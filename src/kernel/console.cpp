#include "console.h"

#include "kernel/arch/interrupts.h"
#include "kernel/arch/serial.h"
#include "kernel/cpu.h"
#include "kernel/framebuffer/term.h"
#include "kernel/input/input.h"
#include "kernel/network/interface.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/scheduler.h"
#include "kernel/smp.h"
#include "kernel/string/format.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "kernel/time/wall_clock.h"
#include "kernel/user_context.h"
#include "user/calendar.h"

namespace FeatherOS::Console
{
namespace
{

constexpr User::TerminalSettings DefaultSettings = {User::TerminalDefaultFlags, 1, 0, 0};

Term* console_term = nullptr;
// Programs on different CPUs share one terminal.
constinit Sync::SpinLock display_lock;

constinit Sync::SpinLock settings_lock;
User::TerminalSettings settings = DefaultSettings;
uint64_t settings_owner = 0;

constinit Sync::SpinLock status_lock;
constinit Scheduler::WaitQueue status_waiters;
bool status_changed = false;
bool status_running = false;
// Readers waiting with TerminalWakeOnOutput.
uint32_t output_watchers = 0;

struct CpuSample
{
    uint64_t ticks[Cpu::MaximumCpus];
    uint64_t busy_ticks[Cpu::MaximumCpus];
};

CpuSample TakeCpuSample()
{
    CpuSample sample;
    for (uint32_t cpu = 0; cpu < Cpu::MaximumCpus; ++cpu)
    {
        sample.ticks[cpu] = Time::CpuTickCount(cpu);
        sample.busy_ticks[cpu] = Scheduler::BusyTickCount(cpu);
    }
    return sample;
}

// A CPU counts as busy when it ran tasks for at least half of the interval.
uint32_t CountBusyCpus(const CpuSample& before, const CpuSample& after)
{
    uint32_t busy_cpus = 0;
    for (uint32_t cpu = 0; cpu < Cpu::MaximumCpus; ++cpu)
    {
        const uint64_t elapsed = after.ticks[cpu] - before.ticks[cpu];
        const uint64_t busy = after.busy_ticks[cpu] - before.busy_ticks[cpu];
        if (elapsed > 0 && busy * 2 >= elapsed)
        {
            ++busy_cpus;
        }
    }
    return busy_cpus;
}

void DrawStatus(uint32_t busy_cpus)
{
    char program[33];
    Process::ForegroundProgramName(program, sizeof(program));
    char status[48];
    const char network[] = {Network::HasRoute() ? Term::StatusGreenDot : Term::StatusRedDot, '\0'};
    bool formatted;
    uint64_t nanoseconds;
    if (Time::ReadWallClock(&nanoseconds))
    {
        const uint64_t seconds = nanoseconds / 1000000000;
        char clock_text[User::ShortCalendarTimeLength];
        User::FormatShortCalendarTime(seconds, clock_text);
        formatted = String::FormatTo(status, sizeof(status), "{}/{}  {}  {}", busy_cpus,
                                     Smp::OnlineCpuCount(), static_cast<const char*>(network),
                                     static_cast<const char*>(clock_text));
    }
    else
    {
        formatted = String::FormatTo(status, sizeof(status), "{}/{}  {}", busy_cpus,
                                     Smp::OnlineCpuCount(), static_cast<const char*>(network));
    }
    if (!formatted)
    {
        Panic("Status bar formatting failed");
    }
    Sync::SpinLockGuard guard(display_lock);
    console_term->SetStatus(program, status);
}

uint64_t StatusTaskMain(void*)
{
    const uint64_t ticks_per_second = Time::TicksPerSecond();
    CpuSample previous = TakeCpuSample();
    uint32_t busy_cpus = 0;
    uint64_t next_sample_tick = Time::TickCount() + ticks_per_second;
    for (;;)
    {
        if (Time::TickCount() >= next_sample_tick)
        {
            const CpuSample current = TakeCpuSample();
            busy_cpus = CountBusyCpus(previous, current);
            previous = current;
            next_sample_tick = Time::TickCount() + ticks_per_second;
        }
        DrawStatus(busy_cpus);
        const uint64_t flags = status_lock.LockIrqSave();
        if (!status_changed)
        {
            Scheduler::WaitOn(status_waiters, status_lock, next_sample_tick);
        }
        status_changed = false;
        status_lock.UnlockIrqRestore(flags);
    }
}

bool InterruptForegroundJob()
{
    if ((Settings().flags & User::TerminalSignals) == 0 || !Process::InterruptForegroundJob())
    {
        return false;
    }
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write("^C\n");
    if (console_term != nullptr)
    {
        console_term->SetCursorVisible(false);
        console_term->Print("^C\n");
    }
    return true;
}

bool CallerInterrupted()
{
    const auto* context = static_cast<const UserExecutionContext*>(Scheduler::CurrentUserContext());
    return context != nullptr && Signals::Deliverable(context->signals) != 0;
}

// Line editing has no use for the cursor and function keys, so their escape sequences are skipped.
enum class SequenceState : uint8_t
{
    None,
    Escape,
    Control,
};

// Returns true when byte belongs to an escape sequence. After a lone Escape key, the next byte
// counts as typed unless it starts a sequence.
bool SkipSequence(SequenceState* state, char byte)
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

uint64_t OutputCount()
{
    return console_term == nullptr ? 0 : console_term->OutputCount();
}

// The kernel log does not wake readers, since it may run under any lock; they look again each
// second instead.
void WakeOutputWatchers()
{
    if (__atomic_load_n(&output_watchers, __ATOMIC_ACQUIRE) != 0)
    {
        Input::WakeWaiters();
    }
}

void StartFreshRow()
{
    Sync::SpinLockGuard guard(display_lock);
    if (console_term != nullptr && console_term->CursorColumn() != 0)
    {
        Serial::Write("\n");
        console_term->Print("\n");
    }
}

void SetCursorVisible(bool visible)
{
    Sync::SpinLockGuard guard(display_lock);
    if (console_term != nullptr)
    {
        console_term->SetCursorVisible(visible);
    }
}

void Echo(const char* text)
{
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write(text);
    if (console_term != nullptr)
    {
        console_term->Print(text);
    }
}

void ShowPrompt(const char* prompt)
{
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write(prompt);
    if (console_term != nullptr)
    {
        console_term->Print(prompt);
        console_term->SetCursorVisible(true);
    }
}

} // namespace

void Initialize(Term* term)
{
    console_term = term;
    Input::SetInterruptHandler(InterruptForegroundJob);
    if (console_term == nullptr || Time::TicksPerSecond() == 0)
    {
        return;
    }
    Scheduler::TaskId status_task;
    if (!Scheduler::CreateTask(StatusTaskMain, nullptr, &status_task) ||
        !Scheduler::SetPriority(status_task, Scheduler::TaskPriority::High))
    {
        Panic("Could not start the status bar task");
    }
    status_running = true;
}

void NotifyStatusChanged()
{
    if (!status_running)
    {
        return;
    }
    const uint64_t flags = status_lock.LockIrqSave();
    status_changed = true;
    Scheduler::WakeAll(status_waiters);
    status_lock.UnlockIrqRestore(flags);
}

void WriteCharacter(char character)
{
    const char text[] = {character, '\0'};
    {
        Sync::SpinLockGuard guard(display_lock);
        Serial::Write(text);
        if (console_term != nullptr)
        {
            console_term->Print(text);
        }
    }
    WakeOutputWatchers();
}

void WriteLine(const char* text)
{
    {
        Sync::SpinLockGuard guard(display_lock);
        Serial::Write(text);
        Serial::Write("\n");
        if (console_term != nullptr)
        {
            console_term->PrintLn(text);
        }
    }
    WakeOutputWatchers();
}

void Write(const char* text, size_t length)
{
    {
        Sync::SpinLockGuard guard(display_lock);
        char chunk[65];
        size_t used = 0;
        auto flush = [&]()
        {
            chunk[used] = '\0';
            Serial::Write(chunk);
            if (console_term != nullptr)
            {
                console_term->Print(chunk);
            }
            used = 0;
        };
        for (size_t index = 0; index < length; ++index)
        {
            const auto byte = static_cast<unsigned char>(text[index]);
            // Escape, carriage return and backspace drive the terminal (see Term); other control
            // bytes are dropped.
            if (byte < 0x20 && byte != '\n' && byte != '\t' && byte != '\x1b' && byte != '\r' &&
                byte != '\b')
            {
                continue;
            }
            chunk[used++] = byte == '\t' ? ' ' : byte > 0x7E ? '?' : static_cast<char>(byte);
            if (used == sizeof(chunk) - 1)
            {
                flush();
            }
        }
        if (used > 0)
        {
            flush();
        }
    }
    WakeOutputWatchers();
}

void Size(uint32_t* rows, uint32_t* columns)
{
    if (console_term == nullptr || console_term->ContentRows() == 0)
    {
        *rows = 24;
        *columns = 80;
        return;
    }
    *rows = console_term->ContentRows();
    *columns = console_term->Columns();
}

bool ReadLine(const char* prompt, char* buffer, size_t capacity, size_t* line_length)
{
    if (buffer == nullptr || capacity == 0 || line_length == nullptr)
    {
        return false;
    }
    if (prompt == nullptr)
    {
        prompt = "";
    }
    ShowPrompt(prompt);

    const bool echo = (Settings().flags & User::TerminalEcho) != 0;
    size_t length = 0;
    buffer[0] = '\0';
    // Syscalls arrive with interrupts masked; timer ticks and keyboard IRQs must run here.
    const uint64_t interrupt_state = Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Enable();
    SequenceState sequence = SequenceState::None;
    bool line_complete = false;
    bool interrupted = false;
    while (!line_complete && !interrupted)
    {
        Input::Poll();
        interrupted = CallerInterrupted();
        char byte;
        // Stop at Enter so input typed ahead stays queued for the next reader.
        while (!line_complete && !interrupted && Input::TryReadByte(&byte))
        {
            if (SkipSequence(&sequence, byte))
            {
                continue;
            }
            if (byte == Input::InterruptByte)
            {
                // Nothing to terminate, so Ctrl+C only discards the line.
                {
                    Sync::SpinLockGuard guard(display_lock);
                    Serial::Write("^C\n");
                    if (console_term != nullptr)
                    {
                        console_term->SetCursorVisible(false);
                        console_term->Print("^C\n");
                    }
                }
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
                    const char text[] = {byte, '\0'};
                    if (echo)
                    {
                        Echo(text);
                    }
                }
            }
            else if (byte == '\x7f' || byte == '\b')
            {
                if (length > 0)
                {
                    buffer[--length] = '\0';
                    Sync::SpinLockGuard guard(display_lock);
                    if (echo)
                    {
                        Serial::Write("\b \b");
                        if (console_term != nullptr)
                        {
                            console_term->EraseCharacter();
                        }
                    }
                }
            }
            else if (byte == '\r' || byte == '\n')
            {
                Sync::SpinLockGuard guard(display_lock);
                if (echo)
                {
                    Serial::Write("\n");
                }
                if (console_term != nullptr)
                {
                    console_term->SetCursorVisible(false);
                    if (echo)
                    {
                        console_term->Print("\n");
                    }
                }
                line_complete = true;
            }
        }
        if (!line_complete && !interrupted)
        {
            Input::WaitForInput(0);
            interrupted = CallerInterrupted();
        }
    }
    if (line_complete)
    {
        Serial::Write("KERN/I: Input line accepted\n");
    }
    Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Restore(interrupt_state);
    *line_length = length;
    return line_complete;
}

bool ReadBytes(char* buffer, size_t capacity, size_t* length)
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
    const uint64_t output_before = OutputCount();
    uint64_t output_seen = output_before;
    uint64_t output_settles_at = 0;
    bool output_arrived = false;
    if (watch_output)
    {
        __atomic_add_fetch(&output_watchers, 1, __ATOMIC_ACQ_REL);
    }
    SetCursorVisible(true);
    const uint64_t interrupt_state = Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Enable();
    size_t count = 0;
    bool interrupted = false;
    for (;;)
    {
        Input::Poll();
        const size_t before = count;
        char byte;
        while (count < capacity && Input::TryReadByte(&byte))
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
            const uint64_t output = OutputCount();
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
        Input::WaitForInput(wake_at);
    }
    Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Restore(interrupt_state);
    if (watch_output)
    {
        __atomic_sub_fetch(&output_watchers, 1, __ATOMIC_ACQ_REL);
    }
    if (output_arrived)
    {
        StartFreshRow();
    }
    SetCursorVisible(false);
    *length = count;
    return count > 0 || !interrupted;
}

User::TerminalSettings Settings()
{
    Sync::SpinLockGuard guard(settings_lock);
    return settings;
}

bool IsDefault(const User::TerminalSettings& candidate)
{
    return candidate.flags == DefaultSettings.flags &&
           candidate.minimum == DefaultSettings.minimum &&
           candidate.timeout == DefaultSettings.timeout;
}

void SetSettings(const User::TerminalSettings& requested, uint64_t owner)
{
    Sync::SpinLockGuard guard(settings_lock);
    settings = requested;
    settings_owner = IsDefault(requested) ? 0 : owner;
}

void ReleaseSettings(uint64_t owner)
{
    {
        Sync::SpinLockGuard guard(settings_lock);
        if (owner == 0 || settings_owner != owner)
        {
            return;
        }
        settings = DefaultSettings;
        settings_owner = 0;
    }
    Sync::SpinLockGuard guard(display_lock);
    if (console_term != nullptr)
    {
        console_term->ResetModes();
    }
}

} // namespace FeatherOS::Console
