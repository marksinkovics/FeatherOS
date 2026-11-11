#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/expected"
#include "libcxx/include/string_view"

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
    friend bool operator==(const Tracked& left, const Tracked& right)
    {
        return left.value == right.value;
    }
};

struct MoveOnly
{
    int value;
    explicit MoveOnly(int initial) : value(initial) {}
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

// Built from a function's result in place, never copied or moved.
struct Pinned
{
    int value;
    constexpr explicit Pinned(int initial) : value(initial) {}
    Pinned(const Pinned&) = delete;
    Pinned& operator=(const Pinned&) = delete;
};

enum class Failure
{
    Negative,
    Odd,
};

constexpr fstd::expected<int, Failure> Half(int value)
{
    if (value < 0)
    {
        return fstd::unexpected(Failure::Negative);
    }
    if (value % 2 != 0)
    {
        return fstd::unexpected(Failure::Odd);
    }
    return value / 2;
}

constexpr fstd::expected<void, Failure> CheckEven(int value)
{
    if (value % 2 != 0)
    {
        return fstd::unexpected(Failure::Odd);
    }
    return {};
}

} // namespace

TEST_CASE("unexpected holds an error", "[libcxx]")
{
    constexpr fstd::unexpected<int> error(3);
    STATIC_REQUIRE(error.error() == 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::unexpected(2.0)), fstd::unexpected<double>>);
    STATIC_REQUIRE(fstd::unexpected(1) == fstd::unexpected(1L) && fstd::unexpected(1) != fstd::unexpected(2));
    STATIC_REQUIRE(!fstd::is_convertible_v<int, fstd::unexpected<int>>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::unexpected<int>(1).error()), int&&>);

    fstd::unexpected<MoveOnly> owned(fstd::in_place, 4);
    fstd::unexpected<MoveOnly> other(MoveOnly(5));
    swap(owned, other);
    REQUIRE((owned.error().value == 5 && other.error().value == 4));
}

TEST_CASE("expected's traits follow its value and error types", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<fstd::expected<int, Failure>>);
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<fstd::expected<void, int>>);
    STATIC_REQUIRE(!fstd::is_trivially_copyable_v<fstd::expected<Tracked, int>>);
    STATIC_REQUIRE(!fstd::is_trivially_destructible_v<fstd::expected<int, Tracked>>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::expected<MoveOnly, int>>);
    STATIC_REQUIRE(fstd::is_move_constructible_v<fstd::expected<int, MoveOnly>>);
    STATIC_REQUIRE(!fstd::is_copy_assignable_v<fstd::expected<void, MoveOnly>>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::expected<Tracked, int>>);
    STATIC_REQUIRE(
        fstd::is_same_v<fstd::expected<int, Failure>::rebind<long>, fstd::expected<long, Failure>>);
}

TEST_CASE("expected constructs a value or an error", "[libcxx]")
{
    STATIC_REQUIRE(Half(8).has_value() && *Half(8) == 4 && Half(8).value() == 4);
    STATIC_REQUIRE(!Half(3) && Half(3).error() == Failure::Odd);
    STATIC_REQUIRE(*fstd::expected<int, int>() == 0);
    STATIC_REQUIRE(fstd::expected<int, int>(fstd::unexpect, 5).error() == 5);
    STATIC_REQUIRE(fstd::is_convertible_v<int, fstd::expected<long, int>>);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::unexpected<short>, fstd::expected<long, int>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::expected<int, int>, fstd::unexpected<const char*>>);

    constexpr fstd::expected<long, Failure> widened = Half(4);
    STATIC_REQUIRE(*widened == 2);
    constexpr fstd::expected<long, Failure> widened_error = Half(1);
    STATIC_REQUIRE(widened_error.error() == Failure::Odd);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::expected<int, int>, fstd::expected<long, long>>);

    // expected<bool> from another expected takes the inner value, not whether one is there.
    fstd::expected<bool, int> flag(fstd::expected<int, int>(0));
    REQUIRE((flag.has_value() && !*flag));

    {
        fstd::expected<Tracked, Tracked> value(fstd::in_place, 1);
        fstd::expected<Tracked, Tracked> error(fstd::unexpect, 2);
        REQUIRE(Tracked::alive == 2);
        fstd::expected<Tracked, Tracked> copy = error;
        fstd::expected<Tracked, Tracked> moved = fstd::move(value);
        REQUIRE((Tracked::alive == 4 && copy.error().value == 2 && moved->value == 1));
    }
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("expected assigns across value and error", "[libcxx]")
{
    {
        fstd::expected<Tracked, Tracked> target(fstd::in_place, 1);
        target = fstd::unexpected(Tracked(2));
        REQUIRE((!target && target.error().value == 2 && Tracked::alive == 1));
        target = Tracked(3);
        REQUIRE((target && target->value == 3 && Tracked::alive == 1));
        fstd::expected<Tracked, Tracked> error(fstd::unexpect, 4);
        target = error;
        REQUIRE((target.error().value == 4 && Tracked::alive == 2));
        target = fstd::expected<Tracked, Tracked>(fstd::in_place, 5);
        REQUIRE((target->value == 5 && Tracked::alive == 2));
        REQUIRE(target.emplace(6).value == 6);
        error.swap(target);
        REQUIRE((error->value == 6 && target.error().value == 4));
        swap(error, target);
        REQUIRE((error.error().value == 4 && target->value == 6 && Tracked::alive == 2));
    }
    REQUIRE(Tracked::alive == 0);

    fstd::expected<MoveOnly, int> owned(fstd::in_place, 1);
    fstd::expected<MoveOnly, int> taken = fstd::unexpected(2);
    taken = fstd::move(owned);
    REQUIRE(taken->value == 1);

    constexpr auto assigned = [] {
        fstd::expected<int, int> value = 1;
        value = fstd::unexpected(2);
        value = fstd::expected<int, int>(3);
        return *value;
    }();
    STATIC_REQUIRE(assigned == 3);
}

TEST_CASE("expected reads values and errors with fallbacks", "[libcxx]")
{
    STATIC_REQUIRE(Half(3).value_or(-1) == -1 && Half(4).value_or(-1) == 2);
    STATIC_REQUIRE(Half(4).error_or(Failure::Negative) == Failure::Negative);
    STATIC_REQUIRE(Half(-2).error_or(Failure::Odd) == Failure::Negative);
    REQUIRE(fstd::expected<MoveOnly, int>(fstd::in_place, 3).value_or(MoveOnly(0)).value == 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(*fstd::expected<int, int>()), int&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::expected<int, long>().error()), long&&>);
    const fstd::expected<int, int> constant = 2;
    STATIC_REQUIRE(fstd::is_same_v<decltype(constant.value()), const int&>);
}

TEST_CASE("expected chains and_then, or_else, transform and transform_error", "[libcxx]")
{
    STATIC_REQUIRE(*Half(8).and_then(Half) == 2);
    STATIC_REQUIRE(Half(6).and_then(Half).error() == Failure::Odd);
    STATIC_REQUIRE(*Half(4).transform([](int value) { return value * 10; }) == 20);
    STATIC_REQUIRE(fstd::is_same_v<decltype(Half(4).transform([](int) { return 1.5; })),
                                   fstd::expected<double, Failure>>);
    STATIC_REQUIRE(
        *Half(3).or_else([](Failure) { return fstd::expected<int, Failure>(-1); }) == -1);
    STATIC_REQUIRE(Half(-1)
                       .transform_error([](Failure failure) { return static_cast<int>(failure); })
                       .error() == 0);
    STATIC_REQUIRE(
        fstd::is_same_v<decltype(Half(1).transform_error([](Failure) { return 'e'; })),
                        fstd::expected<int, char>>);

    auto pinned = Half(14).transform([](int value) { return Pinned(value); });
    REQUIRE(pinned->value == 7);
    auto pinned_error = Half(1).transform_error([](Failure) { return Pinned(9); });
    REQUIRE(pinned_error.error().value == 9);

    fstd::expected<MoveOnly, int> owned(fstd::in_place, 5);
    auto taken = fstd::move(owned).transform([](MoveOnly&& value) { return value.value; });
    REQUIRE(*taken == 5);

    auto to_void = Half(2).transform([](int) {});
    STATIC_REQUIRE(fstd::is_same_v<decltype(to_void), fstd::expected<void, Failure>>);
    REQUIRE(to_void.has_value());
}

TEST_CASE("expected<void> holds nothing or an error", "[libcxx]")
{
    STATIC_REQUIRE(CheckEven(2).has_value() && CheckEven(3).error() == Failure::Odd);
    STATIC_REQUIRE(*CheckEven(4).and_then([] { return Half(6); }) == 3);
    STATIC_REQUIRE(*CheckEven(4).transform([] { return 5; }) == 5);
    STATIC_REQUIRE(CheckEven(1).transform([] { return 5; }).error() == Failure::Odd);
    STATIC_REQUIRE(CheckEven(1).or_else([](Failure) { return CheckEven(2); }).has_value());
    STATIC_REQUIRE(CheckEven(1).error_or(Failure::Negative) == Failure::Odd);
    STATIC_REQUIRE(CheckEven(1)
                       .transform_error([](Failure failure) { return static_cast<int>(failure); })
                       .error() == 1);
    CheckEven(2).value();

    {
        fstd::expected<void, Tracked> value;
        fstd::expected<void, Tracked> error(fstd::unexpect, 1);
        value.swap(error);
        REQUIRE((!value && value.error().value == 1 && error && Tracked::alive == 1));
        value = error;
        REQUIRE((value && Tracked::alive == 0));
        value = fstd::unexpected(Tracked(2));
        REQUIRE((!value && Tracked::alive == 1));
        value.emplace();
        REQUIRE((value && Tracked::alive == 0));
    }
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("expected compares with expected, values and unexpected", "[libcxx]")
{
    STATIC_REQUIRE(Half(4) == Half(4) && Half(4) != Half(8) && Half(1) == Half(3));
    STATIC_REQUIRE(Half(1) != Half(-1) && Half(1) != Half(2));
    STATIC_REQUIRE(Half(4) == 2 && Half(4) != 3 && Half(1) != 0);
    STATIC_REQUIRE(Half(1) == fstd::unexpected(Failure::Odd) && Half(2) != fstd::unexpected(Failure::Odd));
    STATIC_REQUIRE(fstd::expected<int, int>(1) == fstd::expected<long, long>(1));
    STATIC_REQUIRE(CheckEven(2) == CheckEven(4) && CheckEven(1) != CheckEven(2));
    STATIC_REQUIRE(CheckEven(1) == fstd::unexpected(Failure::Odd));

    const fstd::expected<Tracked, int> tracked(fstd::in_place, 1);
    REQUIRE((tracked == Tracked(1) && tracked != Tracked(2)));
}
