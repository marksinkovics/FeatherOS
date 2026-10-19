#include "timer.h"

#if defined(ARCH_x86_64)
#include "kernel/arch/x86_64/timer.h"
#elif defined(ARCH_aarch64)
#include "kernel/arch/aarch64/timer.h"
#endif

#include "kernel/panic.h"
#include "kernel/scheduler.h"

namespace FeatherOS::Time
{
namespace
{

volatile uint64_t ticks = 0;
bool initialized = false;

void OnTimerInterrupt()
{
    ++ticks;
    Scheduler::OnTimerTick();
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

uint64_t TickCount()
{
    return ticks;
}

void WaitForNextTick()
{
    if (!initialized)
    {
        Panic("Cannot wait for an uninitialized timer");
    }
    const uint64_t previous = TickCount();
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