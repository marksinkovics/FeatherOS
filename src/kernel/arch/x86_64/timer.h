#ifndef FEATHEROS_X86_64_TIMER_H
#define FEATHEROS_X86_64_TIMER_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64
{

using TimerInterruptHandler = void (*)();

bool StartPeriodicTimer(uint32_t frequency_hz, TimerInterruptHandler handler);
void StopPeriodicTimer();
bool PrepareSecondaryCpuTimers();
bool StartSecondaryCpuTimer();

} // namespace FeatherOS::Arch::X86_64

#endif