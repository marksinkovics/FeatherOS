#include "timer.h"

#include "interrupts.h"

namespace FeatherOS::Arch::X86_64
{
namespace
{

TimerInterruptHandler timer_handler = nullptr;

void OnPitInterrupt()
{
    if (timer_handler != nullptr)
    {
        timer_handler();
    }
}

} // namespace

bool StartPeriodicTimer(uint32_t frequency_hz, TimerInterruptHandler handler)
{
    if (handler == nullptr)
    {
        return false;
    }
    timer_handler = handler;
    InitializeInterruptController();
    RegisterInterruptHandler(0, OnPitInterrupt);
    StartPit(frequency_hz);
    asm volatile("sti" ::: "memory");
    return true;
}

void StopPeriodicTimer()
{
    StopPit();
    timer_handler = nullptr;
}

} // namespace FeatherOS::Arch::X86_64