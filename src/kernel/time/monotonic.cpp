#include "monotonic.h"

namespace FeatherOS::Time::Monotonic
{
namespace
{

constexpr uint64_t NanosecondsPerSecond = 1000000000;

// Splitting at the second keeps every product below 2^64 for frequencies up to 5 GHz.
uint64_t CounterToNanoseconds(uint64_t elapsed, uint64_t frequency, uint64_t limit)
{
    const uint64_t seconds = elapsed / frequency;
    if (seconds > limit / NanosecondsPerSecond + 1)
    {
        return limit;
    }
    const uint64_t rest = elapsed % frequency;
    return seconds * NanosecondsPerSecond + rest * NanosecondsPerSecond / frequency;
}

} // namespace

uint64_t Interpolate(Sample sample, uint64_t counter_now, uint64_t frequency,
                     uint64_t tick_nanoseconds)
{
    const uint64_t base = sample.ticks * tick_nanoseconds;
    if (frequency == 0 || tick_nanoseconds == 0 || counter_now <= sample.counter)
    {
        return base;
    }
    const uint64_t limit = tick_nanoseconds - 1;
    const uint64_t elapsed = CounterToNanoseconds(counter_now - sample.counter, frequency, limit);
    return base + (elapsed < limit ? elapsed : limit);
}

// Boehm's seqlock: the data is atomic too, so a torn read is a retry, not a data race. The
// release fence keeps the odd sequence ahead of the data stores; the acquire fence keeps the
// data loads ahead of the closing sequence load.
void Clock::Publish(Sample sample)
{
    const uint64_t sequence = __atomic_load_n(&sequence_, __ATOMIC_RELAXED);
    __atomic_store_n(&sequence_, sequence + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&ticks_, sample.ticks, __ATOMIC_RELAXED);
    __atomic_store_n(&counter_, sample.counter, __ATOMIC_RELAXED);
    __atomic_store_n(&sequence_, sequence + 2, __ATOMIC_RELEASE);
}

Sample Clock::Read() const
{
    for (;;)
    {
        const uint64_t before = __atomic_load_n(&sequence_, __ATOMIC_ACQUIRE);
        const Sample sample = {__atomic_load_n(&ticks_, __ATOMIC_RELAXED),
                               __atomic_load_n(&counter_, __ATOMIC_RELAXED)};
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if ((before & 1) == 0 && __atomic_load_n(&sequence_, __ATOMIC_RELAXED) == before)
        {
            return sample;
        }
    }
}

uint64_t Clock::Now(CounterReader read_counter, uint64_t frequency, uint64_t tick_nanoseconds)
{
    // The counter is read after the sample: a tick in between only makes the elapsed part
    // larger, which the cap bounds.
    const Sample sample = Read();
    const uint64_t value = Interpolate(sample, read_counter(), frequency, tick_nanoseconds);
    uint64_t seen = __atomic_load_n(&last_, __ATOMIC_RELAXED);
    while (value > seen)
    {
        if (__atomic_compare_exchange_n(&last_, &seen, value, true, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        {
            return value;
        }
    }
    return seen;
}

void Clock::Reset()
{
    Publish({0, 0});
    __atomic_store_n(&last_, 0, __ATOMIC_RELAXED);
}

} // namespace FeatherOS::Time::Monotonic
