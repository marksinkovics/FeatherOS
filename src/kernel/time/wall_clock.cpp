#include "wall_clock.h"

#include "kernel/arch/rtc.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Time
{
namespace
{

constexpr uint64_t NanosecondsPerSecond = 1000000000;
// Before 2554, so a reading never reaches the syscall error codes at the top of the range.
constexpr uint64_t MaximumNanoseconds = UINT64_MAX / 2;

constinit Sync::SpinLock clock_lock;
bool based = false;
uint64_t base_nanoseconds = 0;
uint64_t base_tick = 0;

bool ReadRealTimeClock(uint64_t* nanoseconds)
{
    uint64_t seconds;
    if (!Arch::RealTimeClock::ReadUnixSeconds(&seconds) ||
        seconds > MaximumNanoseconds / NanosecondsPerSecond)
    {
        return false;
    }
    *nanoseconds = seconds * NanosecondsPerSecond;
    return true;
}

uint64_t ElapsedNanoseconds(uint64_t ticks, uint32_t ticks_per_second)
{
    return ticks / ticks_per_second * NanosecondsPerSecond +
           ticks % ticks_per_second * NanosecondsPerSecond / ticks_per_second;
}

} // namespace

bool ReadWallClock(uint64_t* nanoseconds)
{
    const uint32_t ticks_per_second = TicksPerSecond();
    if (ticks_per_second == 0)
    {
        return ReadRealTimeClock(nanoseconds);
    }
    Sync::SpinLockGuard guard(clock_lock);
    const uint64_t tick = TickCount();
    if (!based)
    {
        if (!ReadRealTimeClock(&base_nanoseconds))
        {
            return false;
        }
        base_tick = tick;
        based = true;
    }
    *nanoseconds = base_nanoseconds + ElapsedNanoseconds(tick - base_tick, ticks_per_second);
    return true;
}

bool SetWallClock(uint64_t nanoseconds)
{
    if (TicksPerSecond() == 0 || nanoseconds > MaximumNanoseconds)
    {
        return false;
    }
    Sync::SpinLockGuard guard(clock_lock);
    base_nanoseconds = nanoseconds;
    base_tick = TickCount();
    based = true;
    return true;
}

} // namespace FeatherOS::Time
