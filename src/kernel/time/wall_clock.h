#ifndef FEATHEROS_TIME_WALL_CLOCK_H
#define FEATHEROS_TIME_WALL_CLOCK_H

#include <stdint.h>

// UTC in nanoseconds since 1970-01-01. Once the timer runs, the clock advances by its ticks from
// one RTC reading, or from the last time it was set; before that it reads the RTC each time and
// cannot be set.
namespace FeatherOS::Time
{

bool ReadWallClock(uint64_t* nanoseconds);
bool SetWallClock(uint64_t nanoseconds);

} // namespace FeatherOS::Time

#endif
