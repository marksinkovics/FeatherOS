#ifndef FEATHEROS_AARCH64_TIMER_H
#define FEATHEROS_AARCH64_TIMER_H

#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{

using TimerInterruptHandler = void (*)();

bool StartPeriodicTimer(uint32_t frequency_hz, const void* device_tree,
                        TimerInterruptHandler handler);
void StopPeriodicTimer();

} // namespace FeatherOS::Arch::AArch64

#endif
