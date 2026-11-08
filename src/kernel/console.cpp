#include "console.h"

#include "kernel/arch/serial.h"
#include "kernel/cpu.h"
#include "kernel/filesystem/devices.h"
#include "kernel/framebuffer/term.h"
#include "kernel/input/input.h"
#include "kernel/network/interface.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/scheduler.h"
#include "kernel/smp.h"
#include "kernel/string/format.h"
#include "kernel/sync/spinlock.h"
#include "kernel/terminal/terminal.h"
#include "kernel/time/timer.h"
#include "kernel/time/wall_clock.h"
#include "user/calendar.h"

namespace FeatherOS::Console
{
namespace
{

// A console's screen. Only the first console also writes to serial.
struct Screen
{
    uint32_t index;
    Term* term;
};

Screen screens[Count] = {{0, nullptr}, {1, nullptr}, {2, nullptr}, {3, nullptr}};
// The consoles share one framebuffer, and programs on different CPUs one screen.
constinit Sync::SpinLock display_lock;
// Changes under display_lock.
uint32_t shown_console GUARDED_BY(display_lock) = 0;

constinit Sync::SpinLock status_lock;
constinit Scheduler::WaitQueue status_waiters;
bool status_changed GUARDED_BY(status_lock) = false;
// Set once at boot, then read without the lock.
bool status_running = false;

Screen& ScreenOf(void* context)
{
    return *static_cast<Screen*>(context);
}

void WriteToDisplayLocked(const Screen& screen, const char* text, size_t length)
{
    char chunk[65];
    while (length > 0)
    {
        const size_t used = length < sizeof(chunk) - 1 ? length : sizeof(chunk) - 1;
        for (size_t index = 0; index < used; ++index)
        {
            chunk[index] = text[index];
        }
        chunk[used] = '\0';
        if (screen.index == 0)
        {
            Serial::Write(chunk);
        }
        if (screen.term != nullptr)
        {
            screen.term->Print(chunk);
        }
        text += used;
        length -= used;
    }
}

void DeviceWrite(void* context, const char* text, size_t length)
{
    Sync::SpinLockGuard guard(display_lock);
    WriteToDisplayLocked(ScreenOf(context), text, length);
}

void DeviceEraseCharacter(void* context)
{
    const Screen& screen = ScreenOf(context);
    Sync::SpinLockGuard guard(display_lock);
    if (screen.index == 0)
    {
        Serial::Write("\b \b");
    }
    if (screen.term != nullptr)
    {
        screen.term->EraseCharacter();
    }
}

void DeviceSetCursorVisible(void* context, bool visible)
{
    const Screen& screen = ScreenOf(context);
    Sync::SpinLockGuard guard(display_lock);
    if (screen.term != nullptr)
    {
        screen.term->SetCursorVisible(visible);
    }
}

void DeviceResetModes(void* context)
{
    const Screen& screen = ScreenOf(context);
    Sync::SpinLockGuard guard(display_lock);
    if (screen.term != nullptr)
    {
        screen.term->ResetModes();
    }
}

uint64_t DeviceOutputCount(void* context)
{
    const Screen& screen = ScreenOf(context);
    return screen.term == nullptr ? 0 : screen.term->OutputCount();
}

void DeviceStartFreshRow(void* context)
{
    const Screen& screen = ScreenOf(context);
    Sync::SpinLockGuard guard(display_lock);
    if (screen.term != nullptr && screen.term->CursorColumn() != 0)
    {
        WriteToDisplayLocked(screen, "\n", 1);
    }
}

void DevicePoll(void*)
{
    Input::Poll();
}

constexpr Terminals::Device MakeDevice(Screen* screen)
{
    return {
        .context = screen,
        .write = DeviceWrite,
        .write_output = nullptr,
        .erase_character = DeviceEraseCharacter,
        .set_cursor_visible = DeviceSetCursorVisible,
        .reset_modes = DeviceResetModes,
        .output_count = DeviceOutputCount,
        .start_fresh_row = DeviceStartFreshRow,
        .poll = DevicePoll,
    };
}

constexpr Terminals::Device ConsoleDevices[Count] = {
    MakeDevice(&screens[0]),
    MakeDevice(&screens[1]),
    MakeDevice(&screens[2]),
    MakeDevice(&screens[3]),
};

// Usable from the start: input may arrive, and be echoed, before Initialize.
constinit Terminals::Terminal console_terminals[Count] = {
    Terminals::Terminal(&ConsoleDevices[0]),
    Terminals::Terminal(&ConsoleDevices[1]),
    Terminals::Terminal(&ConsoleDevices[2]),
    Terminals::Terminal(&ConsoleDevices[3]),
};
constinit Terminals::Terminal* const terminal_pointers[Count] = {
    &console_terminals[0],
    &console_terminals[1],
    &console_terminals[2],
    &console_terminals[3],
};

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
    const uint32_t console = __atomic_load_n(&shown_console, __ATOMIC_ACQUIRE);
    char program[33];
    Process::ForegroundProgramName(console_terminals[console].ForegroundGroup(), program,
                                   sizeof(program));
    char left[40];
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
    if (!formatted || !String::FormatTo(left, sizeof(left), "tty{}  {}", console + 1,
                                        static_cast<const char*>(program)))
    {
        Panic("Status bar formatting failed");
    }
    // Shown again later, the console was drawn with its old bar, and this task notified.
    Sync::SpinLockGuard guard(display_lock);
    screens[console].term->SetStatus(left, status);
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

} // namespace

Terminals::Terminal* Terminal()
{
    return &console_terminals[0];
}

Terminals::Terminal* Terminal(uint32_t console)
{
    return console < Count ? &console_terminals[console] : nullptr;
}

void Initialize(Term* terms)
{
    Devices::SetConsoles(terminal_pointers, Count);
    Input::SetConsoleSwitch(Show);
    if (terms == nullptr)
    {
        return;
    }
    for (uint32_t console = 0; console < Count; ++console)
    {
        Sync::SpinLockGuard guard(display_lock);
        screens[console].term = &terms[console];
        if (terms[console].ContentRows() != 0)
        {
            console_terminals[console].SetSize(terms[console].ContentRows(),
                                               terms[console].Columns());
        }
    }
    if (Time::TicksPerSecond() == 0)
    {
        return;
    }
    Scheduler::TaskId status_task;
    if (!Scheduler::CreateTask(StatusTaskMain, nullptr, &status_task) ||
        !Scheduler::SetPriority(status_task, Scheduler::TaskPriority::High))
    {
        Panic("Could not start the status bar task");
    }
    __atomic_store_n(&status_running, true, __ATOMIC_RELEASE);
}

void Show(uint32_t console)
{
    if (console >= Count)
    {
        return;
    }
    {
        Sync::SpinLockGuard guard(display_lock);
        if (console == shown_console)
        {
            return;
        }
        if (screens[shown_console].term != nullptr)
        {
            screens[shown_console].term->SetShown(false);
        }
        __atomic_store_n(&shown_console, console, __ATOMIC_RELEASE);
        if (screens[console].term != nullptr)
        {
            screens[console].term->SetShown(true);
        }
        Input::SetKeyboardTerminal(&console_terminals[console]);
    }
    NotifyStatusChanged();
}

uint32_t Shown()
{
    return __atomic_load_n(&shown_console, __ATOMIC_ACQUIRE);
}

void NotifyStatusChanged()
{
    if (!__atomic_load_n(&status_running, __ATOMIC_ACQUIRE))
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
    DeviceWrite(&screens[0], &character, 1);
    console_terminals[0].NotifyOutput();
}

void WriteLine(const char* text)
{
    {
        Sync::SpinLockGuard guard(display_lock);
        size_t length = 0;
        while (text[length] != '\0')
        {
            ++length;
        }
        WriteToDisplayLocked(screens[0], text, length);
        WriteToDisplayLocked(screens[0], "\n", 1);
    }
    console_terminals[0].NotifyOutput();
}

#if defined(FEATHEROS_INPUT_TEST)
void WriteSerialLine(const char* text)
{
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write(text);
    Serial::Write("\n");
}
#endif

} // namespace FeatherOS::Console
