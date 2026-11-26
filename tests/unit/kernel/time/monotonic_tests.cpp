#include <catch2/catch_test_macros.hpp>

#include "kernel/time/monotonic.h"

#include <atomic>
#include <thread>
#include <vector>

namespace
{

using FeatherOS::Time::Monotonic::Clock;
using FeatherOS::Time::Monotonic::Interpolate;
using FeatherOS::Time::Monotonic::Sample;

constexpr uint64_t Tick = 10000000;
constexpr uint64_t OneGigahertz = 1000000000;

uint64_t Expected(uint64_t elapsed, uint64_t frequency)
{
    const uint64_t exact =
        static_cast<uint64_t>(static_cast<unsigned __int128>(elapsed) * 1000000000 / frequency);
    return exact < Tick - 1 ? exact : Tick - 1;
}

} // namespace

TEST_CASE("monotonic T9: interpolation at, inside and past a tick", "[monotonic]")
{
    const Sample sample = {7, 5000};
    const uint64_t base = 7 * Tick;
    REQUIRE(Interpolate(sample, 5000, OneGigahertz, Tick) == base);
    REQUIRE(Interpolate(sample, 5000 + 5000000, OneGigahertz, Tick) == base + 5000000);
    REQUIRE(Interpolate(sample, 5000 + Tick - 1, OneGigahertz, Tick) == base + Tick - 1);
    REQUIRE(Interpolate(sample, 5000 + Tick, OneGigahertz, Tick) == base + Tick - 1);
    REQUIRE(Interpolate(sample, 5000 + 5 * Tick, OneGigahertz, Tick) == base + Tick - 1);
    REQUIRE(Interpolate(sample, UINT64_MAX, OneGigahertz, Tick) == base + Tick - 1);
    REQUIRE(Interpolate(sample, UINT64_MAX, 1000000, Tick) == base + Tick - 1);
}

TEST_CASE("monotonic T9: a counter below the sample adds nothing", "[monotonic]")
{
    const Sample sample = {3, 1000000};
    REQUIRE(Interpolate(sample, 999999, OneGigahertz, Tick) == 3 * Tick);
    REQUIRE(Interpolate(sample, 0, OneGigahertz, Tick) == 3 * Tick);
}

TEST_CASE("monotonic T9: no counter gives the tick clock", "[monotonic]")
{
    REQUIRE(Interpolate({9, 0}, 123456789, 0, Tick) == 9 * Tick);
    REQUIRE(Interpolate({0, 0}, 123456789, 0, Tick) == 0);
}

TEST_CASE("monotonic T9: frequencies from 1 MHz to 5 GHz do not overflow", "[monotonic]")
{
    const uint64_t frequencies[] = {1000000,    1193182,    24000000,   62500000,
                                    1000000000, 2400000000, 3000000000, 4999999999,
                                    5000000000};
    for (const uint64_t frequency : frequencies)
    {
        const uint64_t elapsed_counts[] = {0,
                                           1,
                                           frequency / 1000,
                                           frequency / 200,
                                           frequency / 100 - 1,
                                           frequency / 100,
                                           frequency / 100 + 1,
                                           7 * frequency,
                                           UINT64_MAX / 2,
                                           UINT64_MAX - 1};
        for (const uint64_t elapsed : elapsed_counts)
        {
            INFO("frequency " << frequency << " elapsed " << elapsed);
            REQUIRE(Interpolate({1, 0}, elapsed, frequency, Tick) ==
                    Tick + (elapsed == 0 ? 0 : Expected(elapsed, frequency)));
        }
    }
}

TEST_CASE("monotonic: a published sample reads back whole", "[monotonic]")
{
    Clock clock;
    clock.Publish({4, 400});
    const Sample sample = clock.Read();
    REQUIRE(sample.ticks == 4);
    REQUIRE(sample.counter == 400);
    clock.Reset();
    REQUIRE(clock.Read().ticks == 0);
}

TEST_CASE("monotonic: Now keeps the larger of the new and the last value", "[monotonic]")
{
    static std::atomic<uint64_t> counter;
    const auto read_counter = [] { return counter.load(); };
    Clock clock;
    clock.Publish({2, 1000});
    counter = 1000 + 9000000;
    const uint64_t late = clock.Now(read_counter, OneGigahertz, Tick);
    REQUIRE(late == 2 * Tick + 9000000);
    // A reader that sees a counter behind the last one still gets the last value.
    counter = 1000 + 3000000;
    REQUIRE(clock.Now(read_counter, OneGigahertz, Tick) == late);
    clock.Publish({3, 1000 + Tick});
    counter = 1000 + Tick;
    REQUIRE(clock.Now(read_counter, OneGigahertz, Tick) == 3 * Tick);
}

TEST_CASE("monotonic T10: one writer and four readers never see a mixed or backward clock",
          "[monotonic][concurrency]")
{
    constexpr uint64_t PerTick = Tick;
    constexpr uint64_t Ticks = 20000;
    constexpr int Readers = 4;
    static std::atomic<uint64_t> counter;
    static std::atomic<uint64_t> finished_maximum;
    counter = 0;
    finished_maximum = 0;
    const auto read_counter = [] { return counter.load(); };
    Clock clock;
    std::atomic<bool> done{false};
    std::atomic<bool> mixed{false};
    std::atomic<bool> backward{false};

    std::vector<std::thread> threads;
    for (int reader = 0; reader < Readers; ++reader)
    {
        threads.emplace_back(
            [&]
            {
                uint64_t previous = 0;
                while (!done.load())
                {
                    const Sample sample = clock.Read();
                    // The writer only ever publishes counters that are ticks * PerTick.
                    if (sample.counter != sample.ticks * PerTick)
                    {
                        mixed = true;
                    }
                    const uint64_t floor = finished_maximum.load();
                    const uint64_t value = clock.Now(read_counter, OneGigahertz, Tick);
                    uint64_t known = finished_maximum.load();
                    while (value > known && !finished_maximum.compare_exchange_weak(known, value))
                    {
                    }
                    if (value < previous || value < floor)
                    {
                        backward = true;
                    }
                    previous = value;
                }
            });
    }
    for (uint64_t tick = 1; tick <= Ticks; ++tick)
    {
        counter = tick * PerTick;
        clock.Publish({tick, tick * PerTick});
        counter = tick * PerTick + PerTick / 2;
    }
    done = true;
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    REQUIRE_FALSE(mixed.load());
    REQUIRE_FALSE(backward.load());
}
