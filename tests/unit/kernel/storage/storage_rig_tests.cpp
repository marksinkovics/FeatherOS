#include <catch2/catch_test_macros.hpp>

#include "storage_rig.h"

#include <atomic>
#include <thread>

using FeatherOS::Storage::Testing::PumpEnvironment;
using FeatherOS::Storage::Testing::ThreadEnvironment;

namespace
{

bool IsSet(void* flag)
{
    return static_cast<std::atomic<bool>*>(flag)->load();
}

} // namespace

TEST_CASE("the pumping environment runs the pump until woken", "[storage][rig]")
{
    std::atomic<bool> done{false};
    int pumps = 0;
    PumpEnvironment* self = nullptr;
    PumpEnvironment environment(
        [&]
        {
            if (++pumps == 3)
            {
                done = true;
                self->Wake(&done);
            }
        });
    self = &environment;
    const uint64_t start = environment.NowMilliseconds();
    REQUIRE(environment.WaitUntil(&done, start + 1000, IsSet, &done));
    REQUIRE(pumps == 3);
    REQUIRE(environment.Wakes() == 1);
    REQUIRE(environment.NowMilliseconds() == start + 2 * PumpEnvironment::Step);
}

TEST_CASE("the pumping environment times out at the deadline", "[storage][rig]")
{
    std::atomic<bool> never{false};
    PumpEnvironment environment;
    const uint64_t start = environment.NowMilliseconds();
    REQUIRE_FALSE(environment.WaitUntil(&never, start + 25, IsSet, &never));
    REQUIRE(environment.NowMilliseconds() == start + 25);

    environment.Sleep(5);
    REQUIRE(environment.NowMilliseconds() == start + 30);
}

TEST_CASE("the threaded environment wakes a waiter on Wake", "[storage][rig][concurrency]")
{
    ThreadEnvironment environment;
    std::atomic<bool> done{false};
    const uint64_t start = environment.NowMilliseconds();
    std::thread waker(
        [&]
        {
            environment.Sleep(20);
            done = true;
            environment.Wake(&done);
        });
    REQUIRE(environment.WaitUntil(&done, start + 60'000, IsSet, &done));
    REQUIRE(environment.NowMilliseconds() < start + 30'000);
    waker.join();
}

TEST_CASE("the threaded environment times out at the deadline", "[storage][rig][concurrency]")
{
    ThreadEnvironment environment(10);
    std::atomic<bool> never{false};
    const uint64_t start = environment.NowMilliseconds();
    REQUIRE_FALSE(environment.WaitUntil(&never, start + 100, IsSet, &never));
    REQUIRE(environment.NowMilliseconds() >= start + 100);
    REQUIRE(environment.NowMilliseconds() < start + 5000);
}
