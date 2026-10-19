#include "timer.h"

#include "interrupts.h"

#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{
namespace
{

constexpr uint32_t PhysicalTimerInterruptId = 30;
constexpr uint64_t MaximumTimerValue = 0x7FFFFFFF;

TimerInterruptHandler timer_handler = nullptr;
uint64_t timer_interval = 0;

void OnPhysicalTimerInterrupt()
{
    asm volatile("msr CNTP_TVAL_EL0, %0" : : "r"(timer_interval) : "memory");
    if (timer_handler != nullptr)
    {
        timer_handler();
    }
}

} // namespace

bool StartPeriodicTimer(uint32_t frequency_hz, const void* device_tree,
                        TimerInterruptHandler handler)
{
    if (frequency_hz == 0 || handler == nullptr || !InitializeInterruptController(device_tree))
    {
        return false;
    }

    uint64_t counter_frequency;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(counter_frequency));
    if (counter_frequency == 0 || frequency_hz > counter_frequency)
    {
        return false;
    }
    timer_interval = counter_frequency / frequency_hz;
    if (timer_interval == 0 || timer_interval > MaximumTimerValue)
    {
        return false;
    }

    timer_handler = handler;
    RegisterInterruptHandler(PhysicalTimerInterruptId, OnPhysicalTimerInterrupt);

    const uint64_t disabled = 0;
    const uint64_t enabled = 1;
    asm volatile("msr CNTP_CTL_EL0, %0\n\t"
                 "msr CNTP_TVAL_EL0, %1\n\t"
                 "msr CNTP_CTL_EL0, %2\n\t"
                 "dsb sy\n\t"
                 "isb"
                 :
                 : "r"(disabled), "r"(timer_interval), "r"(enabled)
                 : "memory");
    asm volatile("msr daifclr, #2" ::: "memory");
    return true;
}

void StopPeriodicTimer()
{
    const uint64_t disabled = 0;
    asm volatile("msr daifset, #2\n\t"
                 "msr CNTP_CTL_EL0, %0\n\t"
                 "dsb sy\n\t"
                 "isb"
                 :
                 : "r"(disabled)
                 : "memory");
    timer_handler = nullptr;
    timer_interval = 0;
}

} // namespace FeatherOS::Arch::AArch64
