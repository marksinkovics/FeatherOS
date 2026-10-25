#include "console.h"

#include "kernel/arch/interrupts.h"
#include "kernel/arch/rtc.h"
#include "kernel/arch/serial.h"
#include "kernel/cpu.h"
#include "kernel/framebuffer/term.h"
#include "kernel/input/input.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/scheduler.h"
#include "kernel/smp.h"
#include "kernel/string/format.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "kernel/user_context.h"
#include "user/calendar.h"

namespace FeatherOS::Console
{
namespace
{

Term* console_term = nullptr;
// Programs on different CPUs share one terminal.
Sync::SpinLock display_lock;

Sync::SpinLock status_lock;
Scheduler::WaitQueue status_waiters;
bool status_changed = false;
bool status_running = false;
// The bar's clock advances from one RTC read by timer ticks, so it never touches the RTC again.
bool clock_available = false;
uint64_t clock_base_seconds = 0;
uint64_t clock_base_tick = 0;

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
    bool formatted;
    if (clock_available)
    {
        const uint64_t seconds =
            clock_base_seconds + (Time::TickCount() - clock_base_tick) / Time::TicksPerSecond();
        char clock_text[User::ShortCalendarTimeLength];
        User::FormatShortCalendarTime(seconds, clock_text);
        formatted = String::FormatTo(status, sizeof(status), "{}/{}  {}", busy_cpus,
                                     Smp::OnlineCpuCount(), static_cast<const char*>(clock_text));
    }
    else
    {
        formatted =
            String::FormatTo(status, sizeof(status), "{}/{}", busy_cpus, Smp::OnlineCpuCount());
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
    if (!Process::InterruptForegroundJob())
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
    clock_base_tick = Time::TickCount();
    clock_available = Arch::RealTimeClock::ReadUnixSeconds(&clock_base_seconds);
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
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write(text);
    if (console_term != nullptr)
    {
        console_term->Print(text);
    }
}

void WriteLine(const char* text)
{
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write(text);
    Serial::Write("\n");
    if (console_term != nullptr)
    {
        console_term->PrintLn(text);
    }
}

void Write(const char* text, size_t length)
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

    size_t length = 0;
    buffer[0] = '\0';
    // Syscalls arrive with interrupts masked; timer ticks and keyboard IRQs must run here.
    const uint64_t interrupt_state = Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Enable();
    bool line_complete = false;
    bool interrupted = false;
    while (!line_complete && !interrupted)
    {
        Input::Poll();
        Input::Event event;
        interrupted = CallerInterrupted();
        // Stop at Enter so input typed ahead stays queued for the next reader.
        while (!line_complete && !interrupted && Input::TryReadEvent(&event))
        {
            if (event.type == Input::EventType::Interrupt)
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
            Sync::SpinLockGuard guard(display_lock);
            if (event.type == Input::EventType::Character)
            {
                if (length < capacity - 1)
                {
                    buffer[length++] = event.character;
                    buffer[length] = '\0';
                    const char echo[] = {event.character, '\0'};
                    Serial::Write(echo);
                    if (console_term != nullptr)
                    {
                        console_term->Print(echo);
                    }
                }
            }
            else if (event.type == Input::EventType::Backspace)
            {
                if (length > 0)
                {
                    buffer[--length] = '\0';
                    Serial::Write("\b \b");
                    if (console_term != nullptr)
                    {
                        console_term->EraseCharacter();
                    }
                }
            }
            else
            {
                Serial::Write("\n");
                if (console_term != nullptr)
                {
                    console_term->SetCursorVisible(false);
                    console_term->Print("\n");
                }
                line_complete = true;
            }
        }
        if (!line_complete && !interrupted)
        {
            Input::WaitForEvent(0);
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

} // namespace FeatherOS::Console
