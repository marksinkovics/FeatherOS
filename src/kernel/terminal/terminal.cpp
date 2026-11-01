#include "terminal.h"

#include "kernel/arch/interrupts.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Terminals
{
namespace
{

ProcessHooks process_hooks = {nullptr, nullptr, nullptr, nullptr};

bool IsDefault(const User::TerminalSettings& candidate)
{
    return candidate.flags == DefaultSettings.flags &&
           candidate.minimum == DefaultSettings.minimum &&
           candidate.timeout == DefaultSettings.timeout;
}

// Syscalls arrive with interrupts masked; timer ticks and keyboard IRQs must run while a read
// waits.
class InterruptsEnabled
{
public:
    InterruptsEnabled() : state(Arch::Interrupts::SaveAndDisable())
    {
        Arch::Interrupts::Enable();
    }
    ~InterruptsEnabled()
    {
        Arch::Interrupts::SaveAndDisable();
        Arch::Interrupts::Restore(state);
    }
    InterruptsEnabled(const InterruptsEnabled&) = delete;
    InterruptsEnabled& operator=(const InterruptsEnabled&) = delete;

private:
    uint64_t state;
};

} // namespace

// As Linux does: a background change that ignores SIGTTOU goes ahead, so a shell can take the
// terminal back from its job, while a read without SIGTTIN has nothing to wait for.
BackgroundAccess DecideBackgroundAccess(int signal, bool declined, bool orphaned)
{
    if (declined)
    {
        return signal == User::SignalTerminalOutput ? BackgroundAccess::Allowed
                                                    : BackgroundAccess::Fail;
    }
    return orphaned ? BackgroundAccess::Fail : BackgroundAccess::Signal;
}

void SetProcessHooks(const ProcessHooks& hooks)
{
    process_hooks = hooks;
}

bool CallerInterrupted()
{
    return process_hooks.caller_interrupted != nullptr && process_hooks.caller_interrupted();
}

void Terminal::Receive(const char* bytes, size_t length)
{
    if (length == 1 && IsJobSignalByte(bytes[0]))
    {
        DeliverJobSignal(bytes[0]);
        return;
    }
    Sync::SpinLockGuard guard(lock);
    if (length > QueueCapacity - queue_size)
    {
        return;
    }
    for (size_t index = 0; index < length; ++index)
    {
        QueueLocked(bytes[index]);
    }
    Scheduler::WakeAll(readers);
}

size_t Terminal::ReceiveAvailable(const char* bytes, size_t length)
{
    size_t taken = 0;
    while (taken < length)
    {
        if (IsJobSignalByte(bytes[taken]))
        {
            if (!HasRoom())
            {
                break;
            }
            DeliverJobSignal(bytes[taken]);
            ++taken;
            continue;
        }
        Sync::SpinLockGuard guard(lock);
        const size_t start = taken;
        while (taken < length && !IsJobSignalByte(bytes[taken]) && queue_size < QueueCapacity)
        {
            QueueLocked(bytes[taken++]);
        }
        if (taken > start)
        {
            Scheduler::WakeAll(readers);
        }
        if (taken < length && !IsJobSignalByte(bytes[taken]))
        {
            break;
        }
    }
    return taken;
}

void Terminal::WaitForRoom()
{
    const uint64_t flags = lock.LockIrqSave();
    if (queue_size == QueueCapacity && !hung_up)
    {
        ++room_waiters;
        Scheduler::WaitOn(room, lock, 0);
        --room_waiters;
    }
    lock.UnlockIrqRestore(flags);
}

bool Terminal::HasRoom()
{
    Sync::SpinLockGuard guard(lock);
    return queue_size < QueueCapacity;
}

uint64_t Terminal::Write(const char* text, size_t length)
{
    if (HungUp())
    {
        return User::SyscallErrorInputOutput;
    }
    char chunk[128];
    size_t used = 0;
    size_t taken = 0;
    uint64_t failure = 0;
    for (size_t index = 0; index < length && failure == 0; ++index)
    {
        const auto byte = static_cast<unsigned char>(text[index]);
        // Escape, carriage return and backspace drive the screen (see Term); other control bytes
        // are dropped.
        if (byte < 0x20 && byte != '\n' && byte != '\t' && byte != '\x1b' && byte != '\r' &&
            byte != '\b')
        {
            continue;
        }
        chunk[used++] = byte == '\t' ? ' ' : byte > 0x7E ? '?' : static_cast<char>(byte);
        if (used == sizeof(chunk))
        {
            failure = Output(chunk, used);
            used = 0;
            taken = failure == 0 ? index + 1 : taken;
        }
    }
    if (failure == 0 && used > 0)
    {
        failure = Output(chunk, used);
    }
    if (failure == 0)
    {
        taken = length;
    }
    NotifyOutput();
    return taken > 0 || failure == 0 ? taken : failure;
}

// Kernel messages do not wake readers, since they may be logged under any lock; watching readers
// look again each second instead.
void Terminal::NotifyOutput()
{
    if (__atomic_load_n(&output_watchers, __ATOMIC_ACQUIRE) != 0)
    {
        Sync::SpinLockGuard guard(lock);
        Scheduler::WakeAll(readers);
    }
}

uint64_t Terminal::Read(uint64_t group, char* buffer, size_t capacity)
{
    bool buffered;
    {
        Sync::SpinLockGuard guard(lock);
        if (hung_up)
        {
            return 0;
        }
        buffered = line_offset < line_length;
    }
    // A kept line is returned even if the mode changed since it was read.
    if (!buffered)
    {
        if (!InForeground(group))
        {
            return User::SyscallErrorNotForeground;
        }
        if ((Settings().flags & User::TerminalCanonical) == 0)
        {
            return ReadBytes(buffer, capacity);
        }
        char edited[sizeof(line)];
        size_t length;
        if (!EditLine("", edited, sizeof(edited), &length))
        {
            return HungUp() ? 0 : User::SyscallErrorInterrupted;
        }
        edited[length] = '\n';
        Sync::SpinLockGuard guard(lock);
        for (size_t index = 0; index <= length; ++index)
        {
            line[index] = edited[index];
        }
        line_length = static_cast<uint32_t>(length + 1);
        line_offset = 0;
    }
    Sync::SpinLockGuard guard(lock);
    const size_t available = line_length - line_offset;
    const size_t count = capacity < available ? capacity : available;
    for (size_t index = 0; index < count; ++index)
    {
        buffer[index] = line[line_offset + index];
    }
    line_offset += static_cast<uint32_t>(count);
    return count;
}

uint64_t Terminal::ReadLine(uint64_t group, const char* prompt, char* buffer, size_t capacity)
{
    if (buffer == nullptr || capacity == 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (HungUp())
    {
        return User::SyscallErrorInputOutput;
    }
    if (!InForeground(group))
    {
        return User::SyscallErrorNotForeground;
    }
    size_t length;
    if (EditLine(prompt == nullptr ? "" : prompt, buffer, capacity, &length))
    {
        return length;
    }
    return HungUp() ? User::SyscallErrorInputOutput : User::SyscallErrorInterrupted;
}

bool Terminal::HasInput(uint64_t group)
{
    Poll();
    Sync::SpinLockGuard guard(lock);
    return (group == AnyGroup || foreground_group == group) &&
           (line_offset < line_length || queue_size != 0);
}

User::TerminalSettings Terminal::Settings()
{
    Sync::SpinLockGuard guard(lock);
    return settings;
}

void Terminal::SetSettings(const User::TerminalSettings& requested, uint64_t owner)
{
    Sync::SpinLockGuard guard(lock);
    settings = requested;
    settings_owner = IsDefault(requested) ? 0 : owner;
}

void Terminal::ReleaseSettings(uint64_t owner)
{
    {
        Sync::SpinLockGuard guard(lock);
        if (owner == 0 || settings_owner != owner)
        {
            return;
        }
        settings = DefaultSettings;
        settings_owner = 0;
        if (hung_up)
        {
            return;
        }
    }
    device->reset_modes(device->context);
}

void Terminal::Size(uint32_t* row_count, uint32_t* column_count)
{
    Sync::SpinLockGuard guard(lock);
    *row_count = rows;
    *column_count = columns;
}

// The hook takes the process lock, so it runs without the terminal's.
void Terminal::SetSize(uint32_t row_count, uint32_t column_count)
{
    uint64_t group;
    {
        Sync::SpinLockGuard guard(lock);
        if (rows == row_count && columns == column_count)
        {
            return;
        }
        rows = row_count;
        columns = column_count;
        group = foreground_group;
    }
    if (group != 0 && process_hooks.signal_group != nullptr)
    {
        process_hooks.signal_group(group, User::SignalWindowChange);
    }
}

uint64_t Terminal::ForegroundGroup()
{
    Sync::SpinLockGuard guard(lock);
    return foreground_group;
}

void Terminal::SetForegroundGroup(uint64_t group)
{
    Sync::SpinLockGuard guard(lock);
    foreground_group = group;
}

uint64_t Terminal::Session()
{
    Sync::SpinLockGuard guard(lock);
    return session;
}

bool Terminal::Acquire(uint64_t new_session, uint64_t group)
{
    Sync::SpinLockGuard guard(lock);
    if (session != 0 || new_session == 0 || hung_up)
    {
        return false;
    }
    session = new_session;
    foreground_group = group;
    return true;
}

void Terminal::Release(uint64_t old_session)
{
    Sync::SpinLockGuard guard(lock);
    if (old_session != 0 && session == old_session)
    {
        session = 0;
        foreground_group = 0;
    }
}

void Terminal::Reset(const Device* new_device)
{
    Sync::SpinLockGuard guard(lock);
    device = new_device;
    queue_head = 0;
    queue_tail = 0;
    queue_size = 0;
    settings = DefaultSettings;
    settings_owner = 0;
    rows = DefaultRows;
    columns = DefaultColumns;
    foreground_group = 0;
    session = 0;
    hung_up = false;
    line_length = 0;
    line_offset = 0;
}

// The hook takes the process lock, so it runs without the terminal's.
void Terminal::HangUp()
{
    uint64_t old_session;
    {
        Sync::SpinLockGuard guard(lock);
        if (hung_up)
        {
            return;
        }
        hung_up = true;
        old_session = session;
        session = 0;
        foreground_group = 0;
        queue_head = 0;
        queue_tail = 0;
        queue_size = 0;
        line_length = 0;
        line_offset = 0;
        Scheduler::WakeAll(readers);
        Scheduler::WakeAll(room);
    }
    if (old_session != 0 && process_hooks.hang_up_session != nullptr)
    {
        process_hooks.hang_up_session(this, old_session);
    }
}

bool Terminal::HungUp()
{
    Sync::SpinLockGuard guard(lock);
    return hung_up;
}

uint64_t Terminal::AccessGroup(const Caller& caller)
{
    return caller.session != 0 && Session() == caller.session ? caller.group : AnyGroup;
}

uint64_t Terminal::ForegroundGroupFor(const Caller& caller)
{
    Sync::SpinLockGuard guard(lock);
    return session != 0 && session == caller.session ? foreground_group
                                                     : User::SyscallErrorNotTerminal;
}

uint64_t Terminal::ChangeForegroundGroup(const Caller& caller, uint64_t group,
                                         bool group_in_session)
{
    Sync::SpinLockGuard guard(lock);
    if (session == 0 || session != caller.session)
    {
        return User::SyscallErrorNotTerminal;
    }
    if (!group_in_session)
    {
        return User::SyscallErrorNotPermitted;
    }
    foreground_group = group;
    return 0;
}

bool Terminal::InForeground(uint64_t group)
{
    return group == AnyGroup || ForegroundGroup() == group;
}

void Terminal::Poll()
{
    if (device->poll != nullptr)
    {
        device->poll(device->context);
    }
}

bool Terminal::TryReadByte(char* byte)
{
    Sync::SpinLockGuard guard(lock);
    if (queue_size == 0)
    {
        return false;
    }
    *byte = queue[queue_head];
    queue_head = (queue_head + 1) % QueueCapacity;
    --queue_size;
    if (room_waiters != 0)
    {
        Scheduler::WakeAll(room);
    }
    return true;
}

void Terminal::WaitForInput(uint64_t deadline_tick)
{
    const uint64_t flags = lock.LockIrqSave();
    if (queue_size == 0 && !hung_up)
    {
        Scheduler::WaitOn(readers, lock, deadline_tick);
    }
    lock.UnlockIrqRestore(flags);
}

void Terminal::QueueLocked(char byte)
{
    queue[queue_tail] = byte;
    queue_tail = (queue_tail + 1) % QueueCapacity;
    ++queue_size;
}

uint64_t Terminal::Output(const char* text, size_t length)
{
    if (device->write_output == nullptr)
    {
        device->write(device->context, text, length);
        return 0;
    }
    const uint64_t written = device->write_output(device->context, text, length);
    return User::IsSyscallError(written) ? written : 0;
}

// The hook takes the process lock, so it runs without the terminal's.
void Terminal::DeliverJobSignal(char byte)
{
    const bool suspend = byte == SuspendByte;
    if ((Settings().flags & User::TerminalSignals) != 0 &&
        process_hooks.interrupt_group != nullptr &&
        process_hooks.interrupt_group(ForegroundGroup(),
                                      suspend ? User::SignalTerminalStop : User::SignalInterrupt))
    {
        device->set_cursor_visible(device->context, false);
        device->write(device->context, suspend ? "^Z\n" : "^C\n", 3);
        return;
    }
    Sync::SpinLockGuard guard(lock);
    if (queue_size < QueueCapacity)
    {
        QueueLocked(byte);
    }
    Scheduler::WakeAll(readers);
}

bool Terminal::IsJobSignalByte(char byte)
{
    return byte == InterruptByte || byte == SuspendByte;
}

void Terminal::Echo(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    device->write(device->context, text, length);
}

void Terminal::ShowPrompt(const char* prompt)
{
    Echo(prompt);
    device->set_cursor_visible(device->context, true);
}

bool Terminal::EditLine(const char* prompt, char* buffer, size_t capacity, size_t* line_length)
{
    ShowPrompt(prompt);
    const bool echo = (Settings().flags & User::TerminalEcho) != 0;
    size_t length = 0;
    buffer[0] = '\0';
    const InterruptsEnabled interrupts;
    SequenceState sequence = SequenceState::None;
    bool line_complete = false;
    bool interrupted = false;
    while (!line_complete && !interrupted)
    {
        Poll();
        interrupted = CallerInterrupted() || HungUp();
        char byte;
        // Stop at Enter so input typed ahead stays queued for the next reader.
        while (!line_complete && !interrupted && TryReadByte(&byte))
        {
            if (SkipSequence(&sequence, byte))
            {
                continue;
            }
            if (byte == InterruptByte)
            {
                // Nothing to terminate, so Ctrl+C only discards the line.
                device->set_cursor_visible(device->context, false);
                device->write(device->context, "^C\n", 3);
                length = 0;
                buffer[0] = '\0';
                ShowPrompt(prompt);
                continue;
            }
            if (byte >= 0x20 && byte <= 0x7E)
            {
                if (length < capacity - 1)
                {
                    buffer[length++] = byte;
                    buffer[length] = '\0';
                    if (echo)
                    {
                        device->write(device->context, &byte, 1);
                    }
                }
            }
            else if (byte == '\x7f' || byte == '\b')
            {
                if (length > 0)
                {
                    buffer[--length] = '\0';
                    if (echo)
                    {
                        device->erase_character(device->context);
                    }
                }
            }
            else if (byte == '\r' || byte == '\n')
            {
                device->set_cursor_visible(device->context, false);
                if (echo)
                {
                    device->write(device->context, "\n", 1);
                }
                line_complete = true;
            }
        }
        if (!line_complete && !interrupted)
        {
            WaitForInput(0);
            interrupted = CallerInterrupted() || HungUp();
        }
    }
    *line_length = length;
    return line_complete;
}

uint64_t Terminal::ReadBytes(char* buffer, size_t capacity)
{
    const User::TerminalSettings current = Settings();
    const size_t wanted = current.minimum < capacity ? current.minimum : capacity;
    const uint64_t timeout_ticks =
        static_cast<uint64_t>(current.timeout) * Time::TicksPerSecond() / 10;
    // Without a minimum the timeout runs from the start of the read; otherwise from the last byte.
    uint64_t deadline = Time::TickCount() + timeout_ticks;
    const bool watch_output =
        (current.flags & User::TerminalWakeOnOutput) != 0 && current.minimum > 0;
    // Output that comes in bursts, like a log line and a program's message, ends one redraw.
    const uint64_t quiet_ticks = Time::TicksPerSecond() / 10;
    const uint64_t output_before = device->output_count(device->context);
    uint64_t output_seen = output_before;
    uint64_t output_settles_at = 0;
    bool output_arrived = false;
    if (watch_output)
    {
        __atomic_add_fetch(&output_watchers, 1, __ATOMIC_ACQ_REL);
    }
    device->set_cursor_visible(device->context, true);
    size_t count = 0;
    bool interrupted = false;
    {
        const InterruptsEnabled interrupts;
        for (;;)
        {
            Poll();
            const size_t before = count;
            char byte;
            while (count < capacity && TryReadByte(&byte))
            {
                if (byte == '\r' && (current.flags & User::TerminalCarriageReturnToNewline) != 0)
                {
                    byte = '\n';
                }
                buffer[count++] = byte;
            }
            if (count > before)
            {
                if ((current.flags & User::TerminalEcho) != 0)
                {
                    Write(buffer + before, count - before);
                }
                deadline = Time::TickCount() + timeout_ticks;
            }
            const bool timed_out = current.timeout != 0 && Time::TickCount() >= deadline;
            if (current.minimum == 0 ? count > 0 || current.timeout == 0 || timed_out
                                     : count >= wanted || (count > 0 && timed_out))
            {
                break;
            }
            if (HungUp())
            {
                break;
            }
            interrupted = CallerInterrupted();
            if (interrupted)
            {
                break;
            }
            const bool timer_running = current.timeout != 0 && (current.minimum == 0 || count > 0);
            uint64_t wake_at = timer_running ? deadline : 0;
            if (watch_output && count == 0)
            {
                const uint64_t now = Time::TickCount();
                const uint64_t output = device->output_count(device->context);
                if (output != output_seen)
                {
                    output_seen = output;
                    output_settles_at = now + quiet_ticks;
                }
                output_arrived = output_seen != output_before && now >= output_settles_at;
                if (output_arrived)
                {
                    break;
                }
                const uint64_t look_at =
                    output_seen != output_before ? output_settles_at : now + Time::TicksPerSecond();
                wake_at = wake_at == 0 || look_at < wake_at ? look_at : wake_at;
            }
            WaitForInput(wake_at);
        }
    }
    if (watch_output)
    {
        __atomic_sub_fetch(&output_watchers, 1, __ATOMIC_ACQ_REL);
    }
    if (output_arrived)
    {
        device->start_fresh_row(device->context);
    }
    device->set_cursor_visible(device->context, false);
    return count > 0 || !interrupted ? count : User::SyscallErrorInterrupted;
}

// Returns true when byte belongs to an escape sequence. After a lone Escape key, the next byte
// counts as typed unless it starts a sequence.
bool Terminal::SkipSequence(SequenceState* state, char byte)
{
    switch (*state)
    {
    case SequenceState::None:
        break;
    case SequenceState::Escape:
        *state = byte == '[' || byte == 'O' ? SequenceState::Control : SequenceState::None;
        if (*state == SequenceState::Control)
        {
            return true;
        }
        break;
    case SequenceState::Control:
        if (byte >= 0x40 && byte <= 0x7E)
        {
            *state = SequenceState::None;
        }
        return true;
    }
    if (byte == '\x1b')
    {
        *state = SequenceState::Escape;
        return true;
    }
    return false;
}

} // namespace FeatherOS::Terminals
