#include "timer.h"

#if defined(ARCH_x86_64)
#include "kernel/arch/x86_64/timer.h"
#elif defined(ARCH_aarch64)
#include "kernel/arch/aarch64/timer.h"
#endif

#include "kernel/cpu.h"
#include "kernel/input/input.h"
#include "kernel/panic.h"
#include "kernel/scheduler.h"

namespace FeatherOS::Time
{
namespace
{

volatile uint64_t ticks = 0;
volatile uint64_t cpu_ticks[Cpu::MaximumCpus] = {};
bool initialized = false;
uint32_t ticks_per_second = 0;

void OnTimerInterrupt()
{
    const uint32_t cpu_index = Cpu::CurrentIndex();
    cpu_ticks[cpu_index] = cpu_ticks[cpu_index] + 1;
    // Every CPU ticks at the same rate, so one of them is enough to keep global time.
    if (cpu_index == 0)
    {
        ticks = ticks + 1;
        // The UART has no receive interrupt here.
        Input::Poll();
    }
    Scheduler::OnTimerTick(ticks);
}

} // namespace

bool InitializeTimer(uint32_t frequency_hz, const void* platform_data)
{
    if (initialized || frequency_hz == 0)
    {
        return false;
    }
    ticks = 0;
#if defined(ARCH_x86_64)
    (void)platform_data;
    if (!Arch::X86_64::StartPeriodicTimer(frequency_hz, OnTimerInterrupt))
    {
        return false;
    }
#elif defined(ARCH_aarch64)
    if (!Arch::AArch64::StartPeriodicTimer(frequency_hz, platform_data, OnTimerInterrupt))
    {
        return false;
    }
#else
#error "Unsupported architecture for timer initialization"
#endif
    ticks_per_second = frequency_hz;
    initialized = true;
    return true;
}

void StopTimer()
{
    if (!initialized)
    {
        return;
    }
#if defined(ARCH_x86_64)
    Arch::X86_64::StopPeriodicTimer();
#elif defined(ARCH_aarch64)
    Arch::AArch64::StopPeriodicTimer();
#endif
    initialized = false;
}

bool PrepareSecondaryCpuTimers()
{
    if (!initialized)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::PrepareSecondaryCpuTimers();
#elif defined(ARCH_aarch64)
    return Arch::AArch64::PrepareSecondaryCpuTimers();
#endif
}

bool StartSecondaryCpuTimer()
{
    if (!initialized)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::StartSecondaryCpuTimer();
#elif defined(ARCH_aarch64)
    return Arch::AArch64::StartSecondaryCpuTimer();
#endif
}

uint64_t TickCount()
{
    return ticks;
}

uint32_t TicksPerSecond()
{
    return ticks_per_second;
}

uint64_t CpuTickCount(uint32_t cpu_index)
{
    return cpu_index < Cpu::MaximumCpus ? cpu_ticks[cpu_index] : 0;
}

void WaitForNextTick()
{
    if (!initialized)
    {
        Panic("Cannot wait for an uninitialized timer");
    }
    const uint64_t previous = TickCount();
    if (Scheduler::SleepUntil(previous + 1))
    {
        return;
    }
    while (TickCount() == previous)
    {
#if defined(ARCH_x86_64)
        asm volatile("hlt" ::: "memory");
#elif defined(ARCH_aarch64)
        asm volatile("wfi" ::: "memory");
#else
#error "Unsupported architecture for timer wait"
#endif
    }
}

} // namespace FeatherOS::Time