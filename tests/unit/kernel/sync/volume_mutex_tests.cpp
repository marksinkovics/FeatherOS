#include "kernel/scheduler.h"
#include "kernel/sync/mutex.h"
#include "support/scheduler_stubs.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using FeatherOS::Scheduler::CurrentLocal;
using FeatherOS::Sync::VolumeHoldBase;
using FeatherOS::Sync::VolumeMutex;

namespace
{

bool WaitUntil(const std::atomic<int>& value, int at_least)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (value.load() < at_least)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace

TEST_CASE("VolumeMutex: the owner check tells tasks apart", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    VolumeMutex mutex;
    TestScheduler::current_task = 1;
    REQUIRE_FALSE(mutex.HeldByCurrentTask());

    mutex.Lock();
    REQUIRE(mutex.HeldByCurrentTask());
    REQUIRE(CurrentLocal().held_volume == &mutex);

    bool other_task_holds = true;
    std::thread other(
        [&]
        {
            TestScheduler::current_task = 2;
            other_task_holds = mutex.HeldByCurrentTask();
        });
    other.join();
    REQUIRE_FALSE(other_task_holds);

    mutex.Unlock();
    REQUIRE_FALSE(mutex.HeldByCurrentTask());
    REQUIRE(CurrentLocal().held_volume == nullptr);
}

TEST_CASE("VolumeMutex: the boot task is an owner", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    VolumeMutex mutex;
    TestScheduler::current_task = 0;
    mutex.Lock();
    REQUIRE(mutex.HeldByCurrentTask());
    TestScheduler::current_task = 1;
    REQUIRE_FALSE(mutex.HeldByCurrentTask());
    TestScheduler::current_task = 0;
    mutex.Unlock();
    REQUIRE_FALSE(mutex.HeldByCurrentTask());
}

TEST_CASE("VolumeMutex: a second thread waits until Unlock", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    VolumeMutex mutex;
    TestScheduler::current_task = 1;
    mutex.Lock();

    std::atomic<int> got_it = 0;
    std::thread waiter(
        [&]
        {
            TestScheduler::current_task = 2;
            mutex.Lock();
            got_it = 1;
            mutex.Unlock();
        });

    REQUIRE(WaitUntil(TestScheduler::wait_count, 1));
    REQUIRE(got_it.load() == 0);
    mutex.Unlock();
    waiter.join();
    REQUIRE(got_it.load() == 1);
    REQUIRE(TestScheduler::volume_violations == 0);
}

TEST_CASE("VolumeMutex: the per-task slots are distinct", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    CurrentLocal().filesystem_error = 7;

    std::atomic<int> ready = 0;
    std::atomic<int> go = 0;
    std::vector<uint64_t> seen(2);
    std::vector<const void*> addresses(2);
    std::vector<std::thread> threads;
    for (uint64_t index = 0; index < 2; ++index)
    {
        threads.emplace_back(
            [&, index]
            {
                TestScheduler::current_task = static_cast<uint32_t>(index + 2);
                addresses[index] = &CurrentLocal();
                CurrentLocal().filesystem_error = 100 + index;
                ++ready;
                while (go.load() == 0)
                {
                    std::this_thread::yield();
                }
                seen[index] = CurrentLocal().filesystem_error;
            });
    }
    REQUIRE(WaitUntil(ready, 2));
    go = 1;
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    REQUIRE(seen[0] == 100);
    REQUIRE(seen[1] == 101);
    REQUIRE(addresses[0] != addresses[1]);
    REQUIRE(addresses[0] != &CurrentLocal());
    REQUIRE(CurrentLocal().filesystem_error == 7);
}

TEST_CASE("VolumeMutex: a second volume lock is a violation", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    VolumeMutex first;
    VolumeMutex second;
    first.Lock();
    REQUIRE(TestScheduler::volume_violations == 0);
    second.Lock();
    REQUIRE(TestScheduler::volume_violations == 1);
    second.Unlock();
    first.Unlock();

    first.Lock();
    second.LockWithOthers();
    REQUIRE(TestScheduler::volume_violations == 1);
    REQUIRE(first.HeldByCurrentTask());
    REQUIRE(second.HeldByCurrentTask());
    second.Unlock();
    REQUIRE(CurrentLocal().held_volume == &first);
    first.Unlock();
    REQUIRE(CurrentLocal().held_volume == nullptr);
}

TEST_CASE("VolumeMutex: a hold nests on the lock the task has", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    VolumeMutex mutex;
    {
        const VolumeHoldBase outer(mutex);
        REQUIRE(mutex.HeldByCurrentTask());
        {
            const VolumeHoldBase inner(mutex);
            REQUIRE(mutex.HeldByCurrentTask());
        }
        REQUIRE(mutex.HeldByCurrentTask());
    }
    REQUIRE_FALSE(mutex.HeldByCurrentTask());
    REQUIRE(TestScheduler::volume_violations == 0);
}

TEST_CASE("VolumeMutex: holds keep a shared counter exact", "[concurrency][volume_mutex]")
{
    TestScheduler::Reset();
    VolumeMutex mutex;
    uint64_t counter = 0;
    constexpr int threads = 4;
    constexpr int rounds = 500;

    std::vector<std::thread> workers;
    for (int index = 0; index < threads; ++index)
    {
        workers.emplace_back(
            [&, index]
            {
                TestScheduler::current_task = static_cast<uint32_t>(index + 1);
                for (int round = 0; round < rounds; ++round)
                {
                    const VolumeHoldBase hold(mutex);
                    const uint64_t before = counter;
                    std::this_thread::yield();
                    counter = before + 1;
                }
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    REQUIRE(counter == static_cast<uint64_t>(threads) * rounds);
    REQUIRE(TestScheduler::volume_violations == 0);
}
