#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/atomic"

#include <cstring>
#include <thread>
#include <vector>

namespace
{

namespace fstd = featheros_std;

struct Pair
{
    int first;
    int second;
    bool operator==(const Pair&) const = default;
};

// 3 bytes: no instruction updates it, so it takes the lock table.
struct Triple
{
    char values[3];
    bool operator==(const Triple&) const = default;
};

struct Wide
{
    long values[3];
    bool operator==(const Wide&) const = default;
};

struct Padded
{
    char tag;
    // Three bytes of padding sit here.
    int value;
};

static_assert(fstd::atomic<int>::is_always_lock_free && fstd::atomic<long*>::is_always_lock_free);
static_assert(fstd::atomic<Pair>::is_always_lock_free && fstd::atomic<double>::is_always_lock_free);
static_assert(!fstd::atomic<Triple>::is_always_lock_free);
static_assert(!fstd::atomic<Wide>::is_always_lock_free);
static_assert(fstd::atomic<bool>::is_always_lock_free);
static_assert(alignof(fstd::atomic<Pair>) == 8 && sizeof(fstd::atomic<Pair>) == 8);
static_assert(fstd::atomic_ref<Pair>::required_alignment == 8);
static_assert(fstd::atomic_ref<int>::required_alignment == 4);
static_assert(fstd::is_same_v<fstd::atomic<char*>::difference_type, fstd::ptrdiff_t>);
static_assert(fstd::is_same_v<fstd::atomic<float>::difference_type, float>);
static_assert(!fstd::is_copy_constructible_v<fstd::atomic<int>> &&
              !fstd::is_copy_assignable_v<fstd::atomic<int>>);
static_assert(fstd::is_copy_constructible_v<fstd::atomic_ref<int>> &&
              !fstd::is_copy_assignable_v<fstd::atomic_ref<int>>);
static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2);
static_assert(fstd::memory_order_acq_rel == fstd::memory_order::acq_rel);

constinit fstd::atomic<int> constant_counter{7};
constinit fstd::atomic_flag constant_flag = ATOMIC_FLAG_INIT;

template <class Function> void RunThreads(int count, Function function)
{
    std::vector<std::thread> threads;
    for (int index = 0; index < count; ++index)
    {
        threads.emplace_back(function, index);
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
}

} // namespace

TEST_CASE("atomic integers", "[libcxx][atomic]")
{
    fstd::atomic<int> value;
    REQUIRE(value.load() == 0);
    value = 5;
    REQUIRE(static_cast<int>(value) == 5);
    REQUIRE(value.exchange(9, fstd::memory_order_acq_rel) == 5);
    REQUIRE(value.fetch_add(1) == 9);
    REQUIRE(value.fetch_sub(3, fstd::memory_order_relaxed) == 10);
    REQUIRE(++value == 8);
    REQUIRE(value++ == 8);
    REQUIRE(--value == 8);
    REQUIRE(value-- == 8);
    REQUIRE((value += 10) == 17);
    REQUIRE((value -= 2) == 15);
    REQUIRE((value &= 6) == 6);
    REQUIRE((value |= 9) == 15);
    REQUIRE((value ^= 5) == 10);
    REQUIRE(value.fetch_and(3) == 10);
    REQUIRE(value.fetch_or(8) == 2);
    REQUIRE(value.fetch_xor(1) == 10);
    REQUIRE(value.load(fstd::memory_order_acquire) == 11);

    int expected = 4;
    REQUIRE_FALSE(value.compare_exchange_strong(expected, 1));
    REQUIRE(expected == 11);
    REQUIRE(value.compare_exchange_strong(expected, 1, fstd::memory_order_acq_rel,
                                          fstd::memory_order_acquire));
    while (!value.compare_exchange_weak(expected = 1, 2, fstd::memory_order_release))
    {
    }
    REQUIRE(value == 2);

    // Atomic arithmetic wraps, even for signed types.
    fstd::atomic<signed char> small(127);
    REQUIRE(++small == -128);
    fstd::atomic<unsigned> unsigned_value(0);
    REQUIRE((unsigned_value.fetch_sub(1) == 0 && unsigned_value == ~0U));

    REQUIRE(constant_counter.fetch_add(1) == 7);
    REQUIRE(value.is_lock_free());
}

TEST_CASE("atomic pointers, floating point and other types", "[libcxx][atomic]")
{
    long numbers[8] = {};
    fstd::atomic<long*> pointer(numbers);
    REQUIRE(pointer.fetch_add(3) == numbers);
    REQUIRE(pointer.load() == numbers + 3);
    REQUIRE(++pointer == numbers + 4);
    REQUIRE(pointer-- == numbers + 4);
    REQUIRE((pointer += 4) == numbers + 7);
    REQUIRE((pointer -= 7) == numbers);
    REQUIRE(pointer.fetch_sub(-2) == numbers);
    REQUIRE(pointer == numbers + 2);

    fstd::atomic<double> real(1.5);
    REQUIRE(real.fetch_add(2.25) == 1.5);
    REQUIRE((real -= 0.75) == 3.0);
    REQUIRE(real.fetch_sub(4.0) == 3.0);
    REQUIRE(real.load() == -1.0);
    fstd::atomic<float> single;
    REQUIRE(single.load() == 0.0F);

    fstd::atomic<bool> flag_value(false);
    REQUIRE_FALSE(flag_value.exchange(true));
    bool expected = false;
    REQUIRE_FALSE(flag_value.compare_exchange_strong(expected, false));
    REQUIRE(expected);

    fstd::atomic<Pair> pair(Pair{1, 2});
    Pair wanted{1, 2};
    REQUIRE(pair.compare_exchange_strong(wanted, Pair{3, 4}));
    REQUIRE(pair.load() == Pair{3, 4});
    REQUIRE_FALSE(pair.compare_exchange_strong(wanted, Pair{5, 6}));
    REQUIRE(wanted == Pair{3, 4});

    fstd::atomic<Triple> triple(Triple{{'a', 'b', 'c'}});
    REQUIRE_FALSE(triple.is_lock_free());
    REQUIRE(triple.exchange(Triple{{'x', 'y', 'z'}}) == Triple{{'a', 'b', 'c'}});
    Triple seen{{'x', 'y', 'z'}};
    REQUIRE(triple.compare_exchange_weak(seen, Triple{{'1', '2', '3'}}));
    REQUIRE(triple.load() == Triple{{'1', '2', '3'}});
    REQUIRE_FALSE(triple.compare_exchange_strong(seen, Triple{{'4', '5', '6'}}));
    REQUIRE(seen == Triple{{'1', '2', '3'}});

    // Equal values compare equal whatever was in the padding of the expected one.
    fstd::atomic<Padded> padded(Padded{'p', 7});
    Padded padded_expected;
    std::memset(&padded_expected, 0xA5, sizeof(padded_expected));
    padded_expected.tag = 'p';
    padded_expected.value = 7;
    REQUIRE(padded.compare_exchange_strong(padded_expected, Padded{'q', 8}));
    REQUIRE(padded.load().value == 8);
}

TEST_CASE("atomic_ref and atomic_flag", "[libcxx][atomic]")
{
    int counts[2] = {1, 2};
    const fstd::atomic_ref<int> second(counts[1]);
    const fstd::atomic_ref<int> copy = second;
    REQUIRE(second.fetch_add(5) == 2);
    REQUIRE(++copy == 8);
    copy = 3;
    REQUIRE((counts[1] == 3 && counts[0] == 1));

    // The object's padding was never cleared, yet the comparison is of values.
    alignas(fstd::atomic_ref<Padded>::required_alignment) Padded object;
    std::memset(&object, 0x5A, sizeof(object));
    object.tag = 't';
    object.value = 1;
    const fstd::atomic_ref<Padded> padded(object);
    Padded expected{'t', 1};
    REQUIRE(padded.compare_exchange_strong(expected, Padded{'u', 2}));
    REQUIRE((object.tag == 'u' && object.value == 2));
    Padded wrong{'t', 1};
    REQUIRE_FALSE(padded.compare_exchange_strong(wrong, Padded{'v', 3}));
    REQUIRE((wrong.tag == 'u' && wrong.value == 2));

    long* items[1] = {nullptr};
    long storage[4] = {};
    const fstd::atomic_ref<long*> item(items[0]);
    item.store(storage);
    REQUIRE((item += 2) == storage + 2);
    double real = 0.5;
    REQUIRE((fstd::atomic_ref<double>(real).fetch_add(1.0) == 0.5 && real == 1.5));

    REQUIRE_FALSE(constant_flag.test());
    REQUIRE_FALSE(constant_flag.test_and_set());
    REQUIRE(constant_flag.test_and_set(fstd::memory_order_acquire));
    REQUIRE(fstd::atomic_flag_test(&constant_flag));
    fstd::atomic_flag_clear_explicit(&constant_flag, fstd::memory_order_release);
    REQUIRE_FALSE(fstd::atomic_flag_test_and_set(&constant_flag));
    constant_flag.clear();
}

TEST_CASE("atomic free functions", "[libcxx][atomic]")
{
    fstd::atomic<unsigned> value(1);
    fstd::atomic_store(&value, 4U);
    fstd::atomic_store_explicit(&value, 5U, fstd::memory_order_release);
    REQUIRE(fstd::atomic_load(&value) == 5);
    REQUIRE(fstd::atomic_exchange_explicit(&value, 6U, fstd::memory_order_acq_rel) == 5);
    unsigned expected = 6;
    REQUIRE(fstd::atomic_compare_exchange_strong(&value, &expected, 7U));
    REQUIRE_FALSE(fstd::atomic_compare_exchange_weak_explicit(
        &value, &expected, 8U, fstd::memory_order_seq_cst, fstd::memory_order_relaxed));
    REQUIRE(expected == 7);
    REQUIRE(fstd::atomic_fetch_add(&value, 3U) == 7);
    REQUIRE(fstd::atomic_fetch_sub_explicit(&value, 2U, fstd::memory_order_relaxed) == 10);
    REQUIRE(fstd::atomic_fetch_or(&value, 16U) == 8);
    REQUIRE(fstd::atomic_fetch_and(&value, 8U) == 24);
    REQUIRE(fstd::atomic_fetch_xor(&value, 9U) == 8);
    REQUIRE(fstd::atomic_load_explicit(&value, fstd::memory_order_acquire) == 1);
    REQUIRE(fstd::atomic_is_lock_free(&value));
    fstd::atomic_thread_fence(fstd::memory_order_seq_cst);
    fstd::atomic_signal_fence(fstd::memory_order_acq_rel);
    REQUIRE(fstd::kill_dependency(3) == 3);
}

TEST_CASE("atomics under contention", "[libcxx][atomic]")
{
    constexpr int Threads = 4;
    constexpr int Rounds = 20'000;

    fstd::atomic<long> counter;
    fstd::atomic<Wide> wide(Wide{{0, 0, 0}});
    fstd::atomic<double> real;
    fstd::atomic_flag lock;
    long guarded = 0;
    int plain = 0;
    RunThreads(Threads, [&](int index) {
        const fstd::atomic_ref<int> shared(plain);
        for (int round = 0; round < Rounds; ++round)
        {
            counter.fetch_add(1, fstd::memory_order_relaxed);
            shared.fetch_add(index + 1);
            real.fetch_add(1.0);

            Wide current = wide.load(fstd::memory_order_relaxed);
            Wide next;
            do
            {
                next = current;
                ++next.values[0];
                next.values[1] += 2;
                next.values[2] = next.values[0] * 3;
            } while (!wide.compare_exchange_weak(current, next));

            while (lock.test_and_set(fstd::memory_order_acquire))
            {
            }
            ++guarded;
            lock.clear(fstd::memory_order_release);
        }
    });
    REQUIRE(counter == Threads * Rounds);
    REQUIRE(plain == Rounds * (1 + 2 + 3 + 4));
    REQUIRE(real.load() == Threads * Rounds);
    const Wide final_value = wide.load();
    REQUIRE(final_value.values[0] == Threads * Rounds);
    REQUIRE(final_value.values[1] == 2L * Threads * Rounds);
    REQUIRE(final_value.values[2] == 3L * Threads * Rounds);
    REQUIRE(guarded == Threads * Rounds);
}
