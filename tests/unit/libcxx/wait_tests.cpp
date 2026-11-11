#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/atomic"
#include "libcxx/include/barrier"
#include "libcxx/include/latch"
#include "libcxx/include/semaphore"
#include "libcxx/include/thread"

#include <cstring>
#include <vector>

namespace
{

namespace fstd = featheros_std;
using namespace featheros_std::chrono_literals;
using fstd::chrono::milliseconds;
using fstd::chrono::steady_clock;

static_assert(fstd::counting_semaphore<5>::max() == 5 && fstd::binary_semaphore::max() == 1);
static_assert(fstd::counting_semaphore<>::max() == fstd::numeric_limits<fstd::ptrdiff_t>::max());
static_assert(!fstd::is_copy_constructible_v<fstd::latch> && !fstd::is_move_constructible_v<fstd::latch>);
static_assert(fstd::is_nothrow_move_constructible_v<fstd::barrier<>::arrival_token>);
static_assert(fstd::latch::max() > 0 && fstd::barrier<>::max() > 0);

constinit fstd::latch constant_latch(2);
constinit fstd::binary_semaphore constant_semaphore(0);
constinit fstd::barrier<> constant_barrier(1);

milliseconds Since(steady_clock::time_point start)
{
    return fstd::chrono::duration_cast<milliseconds>(steady_clock::now() - start);
}

struct Padded
{
    char tag;
    // Three bytes of padding sit here.
    int value;
};

// 3 bytes: no instruction updates it, so it takes the lock table.
struct Triple
{
    char values[3];
};

// Starts a thread that waits while load() == old and records when it returns, then checks it is
// still waiting after a while, and that change() releases it.
template <class Wait, class Change> void CheckBlocksUntil(Wait wait, Change change)
{
    fstd::atomic<bool> returned{false};
    fstd::thread waiter([&] {
        wait();
        returned = true;
    });
    fstd::this_thread::sleep_for(20ms);
    CHECK_FALSE(returned.load());
    change();
    waiter.join();
    CHECK(returned.load());
}

} // namespace

TEST_CASE("atomic wait returns at once for a changed value", "[libcxx][wait]")
{
    fstd::atomic<int> number(3);
    number.wait(2);
    fstd::atomic<double> real(-0.0);
    // Compared by bits, as compare_exchange compares: -0.0 is not 0.0.
    real.wait(0.0);
    fstd::atomic<Triple> locked(Triple{{'a', 'b', 'c'}});
    CHECK_FALSE(locked.is_lock_free());
    locked.wait(Triple{{'a', 'b', 'd'}});
    int plain = 7;
    fstd::atomic_ref<int>(plain).wait(8);
    fstd::atomic_flag flag;
    flag.wait(true);
    int item = 0;
    fstd::atomic<int*> pointer(&item);
    pointer.wait(nullptr);
    fstd::atomic_wait(&number, 4);
    fstd::atomic_wait_explicit(&number, 4, fstd::memory_order::acquire);
    fstd::atomic_flag_wait(&flag, true);
    fstd::atomic_flag_wait_explicit(&flag, true, fstd::memory_order::relaxed);
}

TEST_CASE("atomic wait sleeps until a notification brings a new value", "[libcxx][wait]")
{
    fstd::atomic<int> number(0);
    CheckBlocksUntil([&] { number.wait(0); },
                     [&] {
                         number.store(1);
                         number.notify_one();
                     });

    fstd::atomic<char> small('x');
    CheckBlocksUntil([&] { small.wait('x', fstd::memory_order::acquire); },
                     [&] {
                         small.store('y', fstd::memory_order::release);
                         fstd::atomic_notify_all(&small);
                     });

    fstd::atomic<Triple> locked(Triple{{'a', 'b', 'c'}});
    CheckBlocksUntil([&] { locked.wait(Triple{{'a', 'b', 'c'}}); },
                     [&] {
                         locked.store(Triple{{'a', 'b', 'z'}});
                         locked.notify_all();
                     });

    // Equal values with different padding still wait.
    Padded stored;
    std::memset(&stored, 0x5a, sizeof(stored));
    stored.tag = 'p';
    stored.value = 1;
    fstd::atomic_ref<Padded> padded(stored);
    Padded old;
    std::memset(&old, 0xa5, sizeof(old));
    old.tag = 'p';
    old.value = 1;
    CheckBlocksUntil([&] { padded.wait(old); },
                     [&] {
                         padded.store(Padded{'p', 2});
                         padded.notify_one();
                     });

    fstd::atomic_flag flag;
    CheckBlocksUntil([&] { flag.wait(false); },
                     [&] {
                         flag.test_and_set();
                         fstd::atomic_flag_notify_one(&flag);
                     });

    // A notification without a change keeps the waiter waiting.
    fstd::atomic<long> unchanged(5);
    CheckBlocksUntil([&] { unchanged.wait(5); },
                     [&] {
                         unchanged.notify_all();
                         fstd::this_thread::sleep_for(20ms);
                         unchanged.store(6);
                         unchanged.notify_all();
                     });
}

TEST_CASE("atomic wait and notify under contention", "[libcxx][wait]")
{
    // Two threads take turns through one word, so a lost wakeup would hang the test.
    constexpr int Rounds = 5000;
    fstd::atomic<int> turn(0);
    fstd::thread other([&] {
        for (int round = 0; round < Rounds; ++round)
        {
            turn.wait(0);
            turn.store(0);
            turn.notify_one();
        }
    });
    for (int round = 0; round < Rounds; ++round)
    {
        turn.store(1);
        turn.notify_one();
        turn.wait(1);
    }
    other.join();
    CHECK(turn.load() == 0);

    // Waiters on neighbouring words, which share futex words, each wake for their own.
    constexpr int Threads = 8;
    fstd::atomic<int> words[Threads] = {};
    fstd::atomic<int> woken(0);
    std::vector<fstd::thread> threads;
    for (int index = 0; index < Threads; ++index)
    {
        threads.emplace_back([&, index] {
            words[index].wait(0);
            ++woken;
        });
    }
    for (int index = 0; index < Threads; ++index)
    {
        fstd::this_thread::sleep_for(2ms);
        words[index].store(1);
        words[index].notify_one();
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(woken.load() == Threads);
}

TEST_CASE("counting_semaphore and binary_semaphore", "[libcxx][wait]")
{
    fstd::counting_semaphore<10> slots(2);
    CHECK(slots.try_acquire());
    CHECK(slots.try_acquire());
    CHECK_FALSE(slots.try_acquire());
    slots.release(2);
    slots.acquire();
    slots.acquire();
    auto start = steady_clock::now();
    CHECK_FALSE(slots.try_acquire_for(30ms));
    CHECK(Since(start) >= 30ms);
    start = steady_clock::now();
    CHECK_FALSE(slots.try_acquire_until(fstd::chrono::system_clock::now() + 20ms));
    CHECK(Since(start) >= 15ms);
    CHECK_FALSE(slots.try_acquire_until(steady_clock::now() - 1s));
    slots.release();
    CHECK(slots.try_acquire_for(fstd::chrono::hours::max()));

    // A producer hands items to four consumers through a counting semaphore.
    constexpr int Items = 4000;
    fstd::counting_semaphore<> available(0);
    fstd::atomic<int> taken(0);
    std::vector<fstd::thread> consumers;
    for (int index = 0; index < 4; ++index)
    {
        consumers.emplace_back([&] {
            for (int item = 0; item < Items / 4; ++item)
            {
                available.acquire();
                ++taken;
            }
        });
    }
    for (int item = 0; item < Items; item += 2)
    {
        available.release(2);
    }
    for (fstd::thread& consumer : consumers)
    {
        consumer.join();
    }
    CHECK(taken.load() == Items);
    CHECK_FALSE(available.try_acquire());

    // A binary semaphore as a lock: the counter is only ever changed by its holder.
    fstd::binary_semaphore lock(1);
    long counter = 0;
    std::vector<fstd::thread> threads;
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back([&] {
            for (int round = 0; round < 2000; ++round)
            {
                lock.acquire();
                ++counter;
                lock.release();
            }
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(counter == 8000);

    fstd::thread releaser([] {
        fstd::this_thread::sleep_for(10ms);
        constant_semaphore.release();
    });
    CHECK(constant_semaphore.try_acquire_for(fstd::chrono::seconds(5)));
    releaser.join();
}

TEST_CASE("latch", "[libcxx][wait]")
{
    fstd::latch done(3);
    CHECK_FALSE(done.try_wait());
    done.count_down();
    done.count_down(2);
    CHECK(done.try_wait());
    done.wait();

    constexpr int Threads = 6;
    fstd::latch started(Threads);
    fstd::latch finished(Threads);
    long values[Threads] = {};
    std::vector<fstd::thread> threads;
    for (int index = 0; index < Threads; ++index)
    {
        threads.emplace_back([&, index] {
            started.arrive_and_wait();
            values[index] = index + 1;
            finished.count_down();
        });
    }
    finished.wait();
    long total = 0;
    for (const long value : values)
    {
        total += value;
    }
    CHECK(total == 21);
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }

    CheckBlocksUntil([] { constant_latch.wait(); }, [] { constant_latch.count_down(2); });
}

TEST_CASE("barrier", "[libcxx][wait]")
{
    constexpr int Threads = 5;
    constexpr int Phases = 200;
    long slots[Threads] = {};
    int completions = 0;
    bool complete_phases = true;
    auto completion = [&]() noexcept {
        // Every thread's write for this round is visible here; each round has two phases.
        for (const long slot : slots)
        {
            complete_phases = complete_phases && slot == completions / 2 + 1;
        }
        ++completions;
    };
    fstd::barrier sync(Threads, completion);
    std::vector<fstd::thread> threads;
    bool seen_completion = true;
    for (int index = 0; index < Threads; ++index)
    {
        threads.emplace_back([&, index] {
            for (int phase = 0; phase < Phases; ++phase)
            {
                slots[index] = phase + 1;
                if (index % 2 == 0)
                {
                    sync.arrive_and_wait();
                }
                else
                {
                    sync.wait(sync.arrive());
                }
                // The completion has run, and nobody writes again before everyone arrives.
                if (index == 0 && completions != 2 * phase + 1)
                {
                    seen_completion = false;
                }
                sync.arrive_and_wait();
            }
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(complete_phases);
    CHECK(seen_completion);
    CHECK(completions == Phases * 2);
}

TEST_CASE("barrier arrive_and_drop", "[libcxx][wait]")
{
    // Each phase, one more thread leaves, until only the main thread is left.
    constexpr int Threads = 4;
    int completions = 0;
    fstd::barrier sync(Threads + 1, [&]() noexcept { ++completions; });
    std::vector<fstd::thread> threads;
    for (int index = 0; index < Threads; ++index)
    {
        threads.emplace_back([&, index] {
            for (int phase = 0; phase < index; ++phase)
            {
                sync.arrive_and_wait();
            }
            sync.arrive_and_drop();
        });
    }
    for (int phase = 0; phase < Threads + 2; ++phase)
    {
        sync.arrive_and_wait();
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(completions == Threads + 2);

    // A barrier of one completes at every arrival, and its tokens are already past.
    auto token = constant_barrier.arrive();
    constant_barrier.wait(std::move(token));
    constant_barrier.arrive_and_wait();
}
