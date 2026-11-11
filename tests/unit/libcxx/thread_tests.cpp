#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/condition_variable"
#include "libcxx/include/mutex"
#include "libcxx/include/shared_mutex"
#include "libcxx/include/stop_token"
#include "libcxx/include/thread"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace
{

namespace fstd = featheros_std;
using namespace featheros_std::chrono_literals;
using fstd::chrono::milliseconds;
using fstd::chrono::steady_clock;

static_assert(!fstd::is_copy_constructible_v<fstd::thread> &&
              fstd::is_nothrow_move_constructible_v<fstd::thread>);
static_assert(fstd::is_nothrow_move_assignable_v<fstd::jthread>);
static_assert(!fstd::is_copy_constructible_v<fstd::mutex> && !fstd::is_move_constructible_v<fstd::mutex>);
static_assert(fstd::is_nothrow_default_constructible_v<fstd::mutex>);
static_assert(fstd::is_same_v<decltype(fstd::scoped_lock(fstd::declval<fstd::mutex&>(),
                                                         fstd::declval<fstd::timed_mutex&>())),
                              fstd::scoped_lock<fstd::mutex, fstd::timed_mutex>>);
static_assert(fstd::is_same_v<fstd::unique_lock<fstd::mutex>::mutex_type, fstd::mutex>);
static_assert(fstd::is_same_v<fstd::scoped_lock<fstd::mutex>::mutex_type, fstd::mutex>);
static_assert(sizeof(fstd::stop_token) == sizeof(void*));
static_assert(fstd::is_default_constructible_v<fstd::scoped_lock<>>);

constinit fstd::mutex constant_mutex;
constinit fstd::once_flag constant_once;

milliseconds Since(steady_clock::time_point start)
{
    return fstd::chrono::duration_cast<milliseconds>(steady_clock::now() - start);
}

struct MoveOnly
{
    explicit MoveOnly(int value) : value(value)
    {
    }
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    int value;
};

template <class Mutex> void Contend(Mutex& mutex)
{
    constexpr int Threads = 4;
    constexpr int Rounds = 2000;
    long counter = 0;
    std::vector<fstd::thread> threads;
    for (int index = 0; index < Threads; ++index)
    {
        threads.emplace_back([&] {
            for (int round = 0; round < Rounds; ++round)
            {
                const fstd::lock_guard<Mutex> guard(mutex);
                ++counter;
            }
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(counter == Threads * Rounds);
}

} // namespace

TEST_CASE("stop_source, stop_token and stop_callback", "[libcxx][thread]")
{
    const fstd::stop_token empty;
    CHECK_FALSE(empty.stop_possible());
    CHECK_FALSE(empty.stop_requested());
    const fstd::stop_source none(fstd::nostopstate);
    CHECK_FALSE(none.stop_possible());

    fstd::stop_token token;
    {
        fstd::stop_source source;
        token = source.get_token();
        CHECK(token.stop_possible());
        CHECK(token == source.get_token());
        CHECK(token != fstd::stop_source().get_token());
    }
    // Its last source is gone, so no stop can come.
    CHECK_FALSE(token.stop_possible());

    fstd::stop_source source;
    fstd::stop_source copy = source;
    token = source.get_token();
    std::vector<int> order;
    std::optional<fstd::stop_callback<std::function<void()>>> removed;
    const fstd::stop_callback first(token, [&] { order.push_back(1); });
    removed.emplace(token, [&] { order.push_back(99); });
    const fstd::stop_callback second(fstd::stop_token(token), [&] { order.push_back(2); });
    removed.reset();
    CHECK(order.empty());
    CHECK(copy.request_stop());
    CHECK_FALSE(source.request_stop());
    CHECK(order == std::vector<int>{2, 1});
    CHECK((token.stop_requested() && token.stop_possible()));

    const fstd::stop_callback late(token, [&] { order.push_back(3); });
    CHECK(order == std::vector<int>{2, 1, 3});
}

TEST_CASE("stop_callback lifetimes across threads", "[libcxx][thread]")
{
    SECTION("a callback that destroys itself")
    {
        fstd::stop_source source;
        std::optional<fstd::stop_callback<std::function<void()>>> callback;
        bool ran = false;
        callback.emplace(source.get_token(), [&] {
            ran = true;
            callback.reset();
        });
        source.request_stop();
        CHECK(ran);
        CHECK_FALSE(callback.has_value());
    }
    SECTION("removing a callback that runs in another thread waits for it")
    {
        fstd::stop_source source;
        fstd::atomic<bool> started{false};
        fstd::atomic<bool> finished{false};
        auto callback = std::make_unique<fstd::stop_callback<std::function<void()>>>(
            source.get_token(), [&] {
                started = true;
                fstd::this_thread::sleep_for(50ms);
                finished = true;
            });
        fstd::thread requester([&] { source.request_stop(); });
        while (!started)
        {
            fstd::this_thread::yield();
        }
        callback.reset();
        CHECK(finished);
        requester.join();
    }
    SECTION("callbacks registered while other threads request stop run once")
    {
        for (int round = 0; round < 50; ++round)
        {
            fstd::stop_source source;
            fstd::atomic<int> runs{0};
            fstd::thread requester([&] { source.request_stop(); });
            {
                const fstd::stop_callback callback(source.get_token(), [&] { ++runs; });
                requester.join();
            }
            CHECK(runs == 1);
        }
    }
}

TEST_CASE("thread and this_thread", "[libcxx][thread]")
{
    fstd::thread empty;
    CHECK_FALSE(empty.joinable());
    CHECK(empty.get_id() == fstd::thread::id());

    int result = 0;
    MoveOnly argument(40);
    fstd::thread::id seen;
    fstd::thread worker(
        [&seen](MoveOnly&& moved, int& out, int extra) {
            seen = fstd::this_thread::get_id();
            out = moved.value + extra;
        },
        std::move(argument), std::ref(result), 2);
    CHECK(worker.joinable());
    const fstd::thread::id id = worker.get_id();
    CHECK(id != fstd::this_thread::get_id());
    fstd::thread moved = std::move(worker);
    CHECK_FALSE(worker.joinable());
    CHECK(moved.get_id() == id);
    moved.join();
    CHECK_FALSE(moved.joinable());
    CHECK(result == 42);
    CHECK(seen == id);
    CHECK(((seen <=> fstd::thread::id()) != 0));
    CHECK(fstd::hash<fstd::thread::id>{}(seen) != fstd::hash<fstd::thread::id>{}({}));

    fstd::atomic<bool> detached_ran{false};
    fstd::thread detached([&] { detached_ran = true; });
    detached.detach();
    CHECK_FALSE(detached.joinable());
    while (!detached_ran)
    {
        fstd::this_thread::yield();
    }

    std::unordered_set<std::size_t> ids;
    std::vector<fstd::thread> threads;
    fstd::mutex ids_mutex;
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back([&] {
            const fstd::lock_guard guard(ids_mutex);
            ids.insert(fstd::hash<fstd::thread::id>{}(fstd::this_thread::get_id()));
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(ids.size() == 4);
    CHECK(fstd::thread::hardware_concurrency() > 0);

    auto start = steady_clock::now();
    fstd::this_thread::sleep_for(20ms);
    CHECK(Since(start) >= 20ms);
    start = steady_clock::now();
    fstd::this_thread::sleep_until(fstd::chrono::system_clock::now() + 20ms);
    CHECK(Since(start) >= 15ms);
    fstd::this_thread::sleep_for(fstd::chrono::hours::min());
    fstd::this_thread::sleep_for(fstd::chrono::duration<double, fstd::milli>(1.5));
}

TEST_CASE("jthread stops and joins", "[libcxx][thread]")
{
    fstd::atomic<int> rounds{0};
    fstd::stop_token kept;
    {
        fstd::jthread worker([&](fstd::stop_token token) {
            while (!token.stop_requested())
            {
                ++rounds;
                fstd::this_thread::yield();
            }
        });
        kept = worker.get_stop_token();
        CHECK(kept.stop_possible());
        while (rounds == 0)
        {
            fstd::this_thread::yield();
        }
    }
    CHECK(kept.stop_requested());

    int sum = 0;
    fstd::jthread plain([&](int left, int right) { sum = left + right; }, 3, 4);
    plain.join();
    CHECK(sum == 7);
    CHECK_FALSE(plain.get_stop_token().stop_requested());

    fstd::jthread first([](fstd::stop_token token) {
        while (!token.stop_requested())
        {
            fstd::this_thread::yield();
        }
    });
    const fstd::stop_token first_token = first.get_stop_token();
    first = fstd::jthread();
    CHECK(first_token.stop_requested());
    CHECK_FALSE(first.joinable());
    CHECK_FALSE(first.get_stop_token().stop_possible());
}

TEST_CASE("mutexes and their locks", "[libcxx][thread]")
{
    Contend(constant_mutex);
    fstd::recursive_mutex recursive;
    Contend(recursive);
    fstd::timed_mutex timed;
    Contend(timed);
    fstd::recursive_timed_mutex recursive_timed;
    Contend(recursive_timed);

    recursive.lock();
    CHECK(recursive.try_lock());
    recursive.unlock();
    bool taken = true;
    fstd::thread([&] { taken = recursive.try_lock(); }).join();
    CHECK_FALSE(taken);
    recursive.unlock();
    fstd::thread([&] {
        taken = recursive.try_lock();
        if (taken)
        {
            recursive.unlock();
        }
    }).join();
    CHECK(taken);

    recursive_timed.lock();
    CHECK(recursive_timed.try_lock_for(1ms));
    recursive_timed.unlock();
    milliseconds waited{};
    fstd::thread([&] {
        const auto start = steady_clock::now();
        taken = recursive_timed.try_lock_for(30ms);
        waited = Since(start);
    }).join();
    CHECK_FALSE(taken);
    CHECK(waited >= 30ms);
    recursive_timed.unlock();

    timed.lock();
    bool any_taken = true;
    milliseconds waited_until{};
    fstd::thread([&] {
        auto start = steady_clock::now();
        any_taken = timed.try_lock_for(30ms);
        waited = Since(start);
        start = steady_clock::now();
        any_taken = any_taken || timed.try_lock_until(fstd::chrono::system_clock::now() + 20ms);
        waited_until = Since(start);
        any_taken = any_taken || timed.try_lock();
    }).join();
    CHECK_FALSE(any_taken);
    CHECK(waited >= 30ms);
    CHECK(waited_until >= 15ms);
    fstd::thread releaser([&] {
        fstd::this_thread::sleep_for(20ms);
        timed.unlock();
    });
    CHECK(timed.try_lock_for(fstd::chrono::seconds(5)));
    releaser.join();
    timed.unlock();

    fstd::mutex mutex;
    fstd::unique_lock<fstd::mutex> deferred(mutex, fstd::defer_lock);
    CHECK_FALSE(deferred.owns_lock());
    deferred.lock();
    CHECK((deferred && deferred.mutex() == &mutex));
    fstd::unique_lock<fstd::mutex> moved = std::move(deferred);
    CHECK((moved.owns_lock() && !deferred.owns_lock() && deferred.mutex() == nullptr));
    fstd::thread([&] {
        const fstd::unique_lock<fstd::mutex> attempt(mutex, fstd::try_to_lock);
        taken = attempt.owns_lock();
    }).join();
    CHECK_FALSE(taken);
    fstd::mutex* released = moved.release();
    CHECK(released == &mutex);
    const fstd::unique_lock<fstd::mutex> adopted(mutex, fstd::adopt_lock);
    CHECK(adopted.owns_lock());

    fstd::unique_lock<fstd::timed_mutex> timed_lock(timed, 10ms);
    CHECK(timed_lock.owns_lock());
    timed_lock.unlock();
    CHECK(timed_lock.try_lock_until(steady_clock::now() + 10ms));
}

TEST_CASE("lock, try_lock and scoped_lock", "[libcxx][thread]")
{
    fstd::mutex first;
    fstd::mutex second;
    fstd::timed_mutex third;

    second.lock();
    int failed = -1;
    fstd::thread([&] { failed = fstd::try_lock(first, second, third); }).join();
    CHECK(failed == 1);
    second.unlock();
    CHECK(first.try_lock());
    first.unlock();
    CHECK(fstd::try_lock(first, second, third) == -1);
    first.unlock();
    second.unlock();
    third.unlock();

    // Opposite orders from different threads would deadlock without the back-off.
    long counter = 0;
    std::vector<fstd::thread> threads;
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back([&, index] {
            for (int round = 0; round < 2000; ++round)
            {
                if (index % 2 == 0)
                {
                    const fstd::scoped_lock lock(first, second, third);
                    ++counter;
                }
                else
                {
                    fstd::lock(third, second, first);
                    ++counter;
                    first.unlock();
                    second.unlock();
                    third.unlock();
                }
            }
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(counter == 8000);

    {
        const fstd::scoped_lock single(first);
        CHECK_FALSE(first.try_lock());
    }
    {
        first.lock();
        second.lock();
        const fstd::scoped_lock adopted(fstd::adopt_lock, first, second);
    }
    CHECK(first.try_lock());
    first.unlock();
}

TEST_CASE("call_once", "[libcxx][thread]")
{
    fstd::atomic<int> calls{0};
    fstd::atomic<int> early_returns{0};
    fstd::once_flag flag;
    std::vector<fstd::thread> threads;
    for (int index = 0; index < 8; ++index)
    {
        threads.emplace_back([&] {
            fstd::call_once(
                flag,
                [&](int step) {
                    fstd::this_thread::sleep_for(10ms);
                    calls += step;
                },
                1);
            // Every caller returns only after the call has finished.
            if (calls != 1)
            {
                ++early_returns;
            }
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(calls == 1);
    CHECK(early_returns == 0);

    int nested = 0;
    fstd::once_flag inner;
    fstd::call_once(constant_once, [&] { fstd::call_once(inner, [&] { ++nested; }); });
    fstd::call_once(constant_once, [&] { ++nested; });
    CHECK(nested == 1);
}

TEST_CASE("shared_mutex and shared_lock", "[libcxx][thread]")
{
    fstd::shared_mutex shared;
    {
        const fstd::shared_lock<fstd::shared_mutex> reader(shared);
        bool shared_taken = false;
        bool exclusive_taken = true;
        fstd::thread([&] {
            shared_taken = shared.try_lock_shared();
            if (shared_taken)
            {
                shared.unlock_shared();
            }
            exclusive_taken = shared.try_lock();
        }).join();
        CHECK(shared_taken);
        CHECK_FALSE(exclusive_taken);
    }
    CHECK(shared.try_lock());
    bool taken = true;
    fstd::thread([&] { taken = shared.try_lock_shared(); }).join();
    CHECK_FALSE(taken);
    shared.unlock();

    // A writer waiting for a reader closes the gate to new readers.
    fstd::shared_timed_mutex timed;
    timed.lock_shared();
    fstd::atomic<bool> writer_done{false};
    fstd::thread writer([&] {
        const fstd::unique_lock<fstd::shared_timed_mutex> lock(timed);
        writer_done = true;
    });
    fstd::this_thread::sleep_for(20ms);
    milliseconds waited{};
    fstd::thread([&] {
        const auto start = steady_clock::now();
        taken = timed.try_lock_shared_for(20ms);
        waited = Since(start);
        taken = taken || timed.try_lock_until(steady_clock::now() + 10ms);
    }).join();
    CHECK_FALSE(taken);
    CHECK(waited >= 20ms);
    CHECK_FALSE(writer_done);
    timed.unlock_shared();
    writer.join();
    CHECK(writer_done);

    // A writer that times out reopens the gate.
    timed.lock_shared();
    fstd::thread([&] { taken = timed.try_lock_for(20ms); }).join();
    CHECK_FALSE(taken);
    fstd::thread([&] {
        taken = timed.try_lock_shared_for(10ms);
        if (taken)
        {
            timed.unlock_shared();
        }
    }).join();
    CHECK(taken);
    timed.unlock_shared();

    long value = 0;
    long reads = 0;
    fstd::mutex reads_mutex;
    std::vector<fstd::thread> threads;
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back([&, index] {
            for (int round = 0; round < 1000; ++round)
            {
                if (index == 0)
                {
                    const fstd::lock_guard<fstd::shared_mutex> lock(shared);
                    ++value;
                    ++value;
                }
                else
                {
                    const fstd::shared_lock<fstd::shared_mutex> lock(shared);
                    const fstd::lock_guard guard(reads_mutex);
                    reads += value % 2;
                }
            }
        });
    }
    for (fstd::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(value == 2000);
    CHECK(reads == 0);
}

TEST_CASE("condition_variable", "[libcxx][thread]")
{
    fstd::mutex mutex;
    fstd::condition_variable ready;
    std::deque<int> queue;
    bool done = false;
    long total = 0;
    fstd::thread consumer([&] {
        fstd::unique_lock<fstd::mutex> lock(mutex);
        while (true)
        {
            ready.wait(lock, [&] { return !queue.empty() || done; });
            if (queue.empty())
            {
                return;
            }
            total += queue.front();
            queue.pop_front();
        }
    });
    for (int value = 1; value <= 1000; ++value)
    {
        {
            const fstd::lock_guard guard(mutex);
            queue.push_back(value);
        }
        ready.notify_one();
    }
    {
        const fstd::lock_guard guard(mutex);
        done = true;
    }
    ready.notify_all();
    consumer.join();
    CHECK(total == 500500);

    fstd::unique_lock<fstd::mutex> lock(mutex);
    auto start = steady_clock::now();
    CHECK(ready.wait_for(lock, 30ms) == fstd::cv_status::timeout);
    CHECK(Since(start) >= 30ms);
    start = steady_clock::now();
    CHECK_FALSE(ready.wait_until(lock, fstd::chrono::system_clock::now() + 20ms, [] { return false; }));
    CHECK(Since(start) >= 15ms);
    CHECK(ready.wait_for(lock, 1h, [] { return true; }));
    lock.unlock();

    bool signalled = false;
    fstd::thread notifier([&] {
        fstd::unique_lock<fstd::mutex> held(mutex);
        signalled = true;
        fstd::notify_all_at_thread_exit(ready, std::move(held));
    });
    lock.lock();
    CHECK(ready.wait_for(lock, fstd::chrono::seconds(5), [&] { return signalled; }));
    lock.unlock();
    notifier.join();
}

TEST_CASE("condition_variable_any", "[libcxx][thread]")
{
    fstd::shared_mutex shared;
    fstd::condition_variable_any changed;
    int value = 0;
    fstd::thread waiter([&] {
        fstd::shared_lock<fstd::shared_mutex> lock(shared);
        changed.wait(lock, [&] { return value == 3; });
    });
    for (int step = 0; step < 3; ++step)
    {
        {
            const fstd::lock_guard<fstd::shared_mutex> guard(shared);
            ++value;
        }
        changed.notify_all();
    }
    waiter.join();

    fstd::timed_mutex timed;
    fstd::unique_lock<fstd::timed_mutex> lock(timed);
    const auto start = steady_clock::now();
    CHECK(changed.wait_for(lock, 20ms) == fstd::cv_status::timeout);
    CHECK(Since(start) >= 20ms);
    CHECK(lock.owns_lock());
    lock.unlock();

    // A stop request wakes an interruptible wait, which then returns the predicate's value.
    fstd::mutex mutex;
    bool woken_result = true;
    fstd::jthread stopped([&](fstd::stop_token token) {
        fstd::unique_lock<fstd::mutex> held(mutex);
        woken_result = changed.wait(held, token, [] { return false; });
    });
    fstd::this_thread::sleep_for(20ms);
    stopped.request_stop();
    stopped.join();
    CHECK_FALSE(woken_result);

    fstd::stop_source source;
    fstd::unique_lock<fstd::mutex> held(mutex);
    CHECK_FALSE(changed.wait_for(held, source.get_token(), 20ms, [] { return false; }));
    source.request_stop();
    CHECK(changed.wait(held, source.get_token(), [] { return true; }));
    CHECK_FALSE(changed.wait_until(held, source.get_token(), steady_clock::now() + 1h,
                                   [] { return false; }));
}
