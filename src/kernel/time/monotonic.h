#ifndef FEATHEROS_TIME_MONOTONIC_H
#define FEATHEROS_TIME_MONOTONIC_H

#include <stdint.h>

// The arithmetic of the fine clock, with no hardware in it: monotonic time is the tick clock
// plus the counter's reading since the last tick, capped just below one tick.
namespace FeatherOS::Time::Monotonic
{

struct Sample
{
    uint64_t ticks;
    uint64_t counter;
};

// Zero frequency means no counter: the result is the tick clock. A counter below the sample
// counts as no time passed. The result is below (ticks + 1) * tick_nanoseconds.
uint64_t Interpolate(Sample sample, uint64_t counter_now, uint64_t frequency,
                     uint64_t tick_nanoseconds);

using CounterReader = uint64_t (*)();

// One writer publishes samples (CPU 0's tick handler); any CPU reads.
class Clock
{
public:
    void Publish(Sample sample);
    Sample Read() const;
    // Never returns less than an earlier call did, on any CPU.
    uint64_t Now(CounterReader read_counter, uint64_t frequency, uint64_t tick_nanoseconds);
    void Reset();

private:
    // Even while stable, odd while the writer is inside Publish.
    uint64_t sequence_ = 0;
    uint64_t ticks_ = 0;
    uint64_t counter_ = 0;
    uint64_t last_ = 0;
};

} // namespace FeatherOS::Time::Monotonic

#endif
