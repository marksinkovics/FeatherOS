#ifndef FEATHEROS_TIME_TIMER_H
#define FEATHEROS_TIME_TIMER_H

#include <stdint.h>

namespace FeatherOS::Time
{

bool InitializeTimer(uint32_t frequency_hz, const void* platform_data = nullptr);
void StopTimer();
uint64_t TickCount();
void WaitForNextTick();

} // namespace FeatherOS::Time

#endif