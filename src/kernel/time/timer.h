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
// CLOCK_MONOTONIC in nanoseconds: the tick clock plus the fine counter's time since CPU 0's last
// tick, never more than one tick ahead of it; 0 before the timer runs. Tick resolution where the
// counter is not usable.
uint64_t MonotonicNanoseconds();
uint64_t CpuTickCount(uint32_t cpu_index);
void WaitForNextTick();

} // namespace FeatherOS::Time

#endif