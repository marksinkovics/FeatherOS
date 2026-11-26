#include "kernel/arch/fine_counter.h"

#include "kernel/time/timer.h"

namespace FeatherOS::Arch::FineCounter
{
namespace
{

constexpr uint32_t CalibrationTicks = 10;

uint64_t frequency = 0;

void Cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ecx, uint32_t* edx)
{
    uint32_t ebx;
    asm volatile("cpuid" : "=a"(*eax), "=b"(ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf), "c"(0));
}

// An invariant TSC ticks at one rate in every power state and is synchronised across cores. A
// hypervisor's TSC is trusted because QEMU's does not advertise the bit under TCG.
bool TscIsTrusted()
{
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    Cpuid(1, &eax, &ecx, &edx);
    if ((ecx & (1U << 31)) != 0)
    {
        return true;
    }
    Cpuid(0x80000000, &eax, &ecx, &edx);
    if (eax < 0x80000007)
    {
        return false;
    }
    Cpuid(0x80000007, &eax, &ecx, &edx);
    return (edx & (1U << 8)) != 0;
}

void WaitForTick(uint64_t target)
{
    while (Time::TickCount() < target)
    {
        asm volatile("hlt" ::: "memory");
    }
}

} // namespace

bool Initialize()
{
    if (frequency != 0)
    {
        return true;
    }
    if (!TscIsTrusted() || Time::TicksPerSecond() == 0)
    {
        return false;
    }
    WaitForTick(Time::TickCount() + 1);
    const uint64_t first_tick = Time::TickCount();
    const uint64_t start = Read();
    WaitForTick(first_tick + CalibrationTicks);
    const uint64_t elapsed_ticks = Time::TickCount() - first_tick;
    const uint64_t counted = Read() - start;
    frequency = counted * Time::TicksPerSecond() / elapsed_ticks;
    return frequency != 0;
}

uint64_t Read()
{
    uint32_t low;
    uint32_t high;
    // LFENCE stops RDTSC running ahead of earlier instructions.
    asm volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

uint64_t Frequency()
{
    return frequency;
}

} // namespace FeatherOS::Arch::FineCounter
