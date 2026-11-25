#ifndef FEATHEROS_TIME_TIMER_H
#define FEATHEROS_TIME_TIMER_H

#include <stdint.h>

namespace FeatherOS::Time
{

bool InitializeTimer(uint32_t frequency_hz, const void* platform_data = nullptr);
void StopTimer();
// True from InitializeTimer until StopTimer.
bool TimerRunning();
bool PrepareSecondaryCpuTimers();
bool StartSecondaryCpuTimer();
uint64_t TickCount();
uint32_t TicksPerSecond();
// CLOCK_MONOTONIC in milliseconds; 0 before the timer runs.
uint64_t UptimeMilliseconds();
uint64_t CpuTickCount(uint32_t cpu_index);
void WaitForNextTick();

} // namespace FeatherOS::Time

#endif