#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/optional"
#include "libcxx/include/string_view"

#include <cstdio>
#include <cstdlib>

// The host build's abort hook; the C library and the kernel define the real ones.
void featheros_std::__detail::LibraryAbort(const char* message) noexcept
{
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

namespace
{

namespace fstd = featheros_std;

struct Tracked
{
    static inline int alive = 0;
    int value;
    explicit Tracked(int initial) : value(initial) { ++alive; }
    Tracked(const Tracked& other) : value(other.value) { ++alive; }
    Tracked(Tracked&& other) noexcept : value(other.value)
    {
        other.value = -1;
        ++alive;
    }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
};

struct MoveOnly
{
    int value;
    explicit MoveOnly(int initial) : value(initial) {}
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

struct NotCopyable
{
    NotCopyable() = default;
    NotCopyable(const NotCopyable&) = delete;
    NotCopyable& operator=(const NotCopyable&) = delete;
};

// Built from a function's result in place, never copied or moved.
struct Pinned
{
    int value;
    explicit Pinned(int initial) : value(initial) {}
    Pinned(const Pinned&) = delete;
    Pinned& operator=(const Pinned&) = delete;
};

struct Listed
{
    int count;
    int extra;
    Listed(fstd::initializer_list<int> list, int more)
        : count(static_cast<int>(list.size())), extra(more)
    {
    }
};

constexpr fstd::optional<int> Half(int value)
{
    if (value % 2 != 0)
    {
        return fstd::nullopt;
    }
    return value / 2;
}

} // namespace

TEST_CASE("optional is trivial when its value is", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<fstd::optional<int>>);
    STATIC_REQUIRE(fstd::is_trivially_destructible_v<fstd::optional<int>>);
    STATIC_REQUIRE(!fstd::is_trivially_destructible_v<fstd::optional<Tracked>>);
    STATIC_REQUIRE(!fstd::is_trivially_copy_constructible_v<fstd::optional<Tracked>>);
    STATIC_REQUIRE(fstd::is_copy_constructible_v<fstd::optional<Tracked>>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::optional<MoveOnly>>);
    STATIC_REQUIRE(fstd::is_move_constructible_v<fstd::optional<MoveOnly>>);
    STATIC_REQUIRE(!fstd::is_copy_assignable_v<fstd::optional<MoveOnly>>);
    STATIC_REQUIRE(!fstd::is_move_constructible_v<fstd::optional<NotCopyable>>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::optional<Tracked>>);
    STATIC_REQUIRE(!fstd::is_assignable_v<fstd::optional<const int>&, int>);
}

TEST_CASE("optional constructs, converts and destroys", "[libcxx]")
{
    constexpr fstd::optional<int> empty;
    STATIC_REQUIRE(!empty.has_value() && !empty && empty == fstd::nullopt);
    constexpr fstd::optional<int> three = 3;
    STATIC_REQUIRE(three.has_value() && *three == 3 && three.value() == 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::optional{2.5}), fstd::optional<double>>);
    STATIC_REQUIRE(Half(4) == 2 && Half(3) == fstd::nullopt);

    constexpr fstd::optional<long> widened = three;
    STATIC_REQUIRE(*widened == 3L);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::optional<int>, fstd::optional<long>>);
    STATIC_REQUIRE(!fstd::is_convertible_v<int, fstd::optional<Tracked>>);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::optional<Tracked>, int>);

    // optional<bool> from optional<int> takes the inner value, not whether one is there.
    fstd::optional<bool> flag(fstd::optional<int>(0));
    REQUIRE((flag.has_value() && !*flag));

    {
        fstd::optional<Tracked> tracked(fstd::in_place, 5);
        REQUIRE(Tracked::alive == 1);
        fstd::optional<Tracked> copy = tracked;
        REQUIRE((Tracked::alive == 2 && copy->value == 5));
        fstd::optional<Tracked> moved = fstd::move(tracked);
        REQUIRE((Tracked::alive == 3 && moved->value == 5 && tracked->value == -1));
        tracked.reset();
        REQUIRE((Tracked::alive == 2 && !tracked));
    }
    REQUIRE(Tracked::alive == 0);

    fstd::optional<Listed> listed(fstd::in_place, {1, 2, 3}, 9);
    REQUIRE((listed->count == 3 && listed->extra == 9));
    auto made = fstd::make_optional<Listed>({1, 2}, 4);
    REQUIRE(made->count == 2);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::make_optional(1)), fstd::optional<int>>);
    REQUIRE(fstd::make_optional<Tracked>(6)->value == 6);
}

TEST_CASE("optional assigns, emplaces and swaps", "[libcxx]")
{
    fstd::optional<Tracked> target;
    fstd::optional<Tracked> source(fstd::in_place, 1);
    target = source;
    REQUIRE((target->value == 1 && Tracked::alive == 2));
    target = fstd::nullopt;
    REQUIRE((!target && Tracked::alive == 1));
    target = Tracked(4);
    REQUIRE(target->value == 4);
    target = fstd::optional<Tracked>();
    REQUIRE(!target);
    REQUIRE(target.emplace(8).value == 8);
    source.reset();
    swap(target, source);
    REQUIRE((!target && source->value == 8));
    target.emplace(2);
    target.swap(source);
    REQUIRE((target->value == 8 && source->value == 2));
    target.reset();
    source.reset();
    REQUIRE(Tracked::alive == 0);

    fstd::optional<long> widened;
    widened = fstd::optional<int>(7);
    REQUIRE(*widened == 7);

    fstd::optional<MoveOnly> movable;
    movable = MoveOnly(3);
    fstd::optional<MoveOnly> other = fstd::move(movable);
    REQUIRE(other->value == 3);

    constexpr auto assigned = [] {
        fstd::optional<int> value;
        value = 4;
        value = fstd::optional<int>(5);
        return *value;
    }();
    STATIC_REQUIRE(assigned == 5);
}

TEST_CASE("optional reads values with fallbacks", "[libcxx]")
{
    fstd::optional<int> empty;
    REQUIRE(empty.value_or(9) == 9);
    REQUIRE(fstd::optional<int>(1).value_or(9) == 1);
    REQUIRE(fstd::optional<MoveOnly>(fstd::in_place, 3).value_or(MoveOnly(0)).value == 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(*fstd::optional<int>()), int&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::optional<int>().value()), int&&>);
    const fstd::optional<int> constant = 2;
    STATIC_REQUIRE(fstd::is_same_v<decltype(*constant), const int&>);
}

TEST_CASE("optional chains and_then, transform and or_else", "[libcxx]")
{
    STATIC_REQUIRE(Half(8).and_then(Half) == 2);
    STATIC_REQUIRE(!Half(6).and_then(Half).has_value());
    STATIC_REQUIRE(Half(4).transform([](int value) { return value * 10; }) == 20);
    STATIC_REQUIRE(fstd::is_same_v<decltype(Half(4).transform([](int) { return 1.5; })),
                                   fstd::optional<double>>);
    STATIC_REQUIRE(Half(3).or_else([] { return fstd::optional<int>(-1); }) == -1);
    STATIC_REQUIRE(Half(2).or_else([] { return fstd::optional<int>(-1); }) == 1);

    auto pinned = fstd::optional<int>(7).transform([](int value) { return Pinned(value); });
    REQUIRE(pinned->value == 7);
    fstd::optional<int> empty;
    REQUIRE(!empty.transform([](int value) { return Pinned(value); }).has_value());

    fstd::optional<MoveOnly> owned(fstd::in_place, 5);
    auto taken = fstd::move(owned).transform([](MoveOnly&& value) { return value.value; });
    REQUIRE(taken == 5);

    fstd::optional<fstd::string_view> word = "chain";
    REQUIRE(word.transform([](fstd::string_view text) { return text.size(); }) == 5u);
}

TEST_CASE("optional compares with optionals, nullopt and values", "[libcxx]")
{
    constexpr fstd::optional<int> empty;
    constexpr fstd::optional<int> one = 1;
    constexpr fstd::optional<long> two = 2;
    STATIC_REQUIRE(empty == empty && one != empty && one != two && one == fstd::optional<long>(1));
    STATIC_REQUIRE(empty < one && one < two && two > one && empty <= empty && two >= one);
    STATIC_REQUIRE((empty <=> one) < 0 && (two <=> one) > 0 && (empty <=> empty) == 0);
    STATIC_REQUIRE(empty == fstd::nullopt && fstd::nullopt == empty && one != fstd::nullopt);
    STATIC_REQUIRE(fstd::nullopt < one && !(one < fstd::nullopt) && (one <=> fstd::nullopt) > 0);
    STATIC_REQUIRE(one == 1 && 1 == one && one != 2 && empty != 1);
    STATIC_REQUIRE(empty < 0 && 0 > empty && one <= 1 && 2 >= one && one < 2L);
    STATIC_REQUIRE((one <=> 0) > 0 && (empty <=> 0) < 0 && (0 <=> one) < 0);

    fstd::optional<double> nan = __builtin_nan("");
    REQUIRE(!(nan == nan));
    REQUIRE(fstd::is_eq(fstd::optional<double>() <=> fstd::optional<double>()));
}
