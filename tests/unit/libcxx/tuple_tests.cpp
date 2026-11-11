#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/array"
#include "libcxx/include/functional"
#include "libcxx/include/tuple"

namespace
{

namespace fstd = featheros_std;

struct Empty
{
};

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

struct ExplicitOnly
{
    int value;
    constexpr explicit ExplicitOnly(int initial) : value(initial) {}
};

// Ordered only through <, so tuple's <=> falls back to a weak ordering.
struct LessOnly
{
    int value;
    friend constexpr bool operator<(LessOnly left, LessOnly right)
    {
        return left.value < right.value;
    }
};

struct NotComparable
{
};

struct Point
{
    int x;
    int y;
    constexpr Point(int first, int second) : x(first), y(second) {}
};

struct Arena
{
    int id;
};

// Records the arena it was built with, if any.
struct ArenaUser
{
    using allocator_type = Arena;
    int value = 0;
    int arena = 0;
    constexpr ArenaUser() = default;
    constexpr explicit ArenaUser(int initial) : value(initial) {}
    constexpr ArenaUser(fstd::allocator_arg_t, Arena allocator) : arena(allocator.id) {}
    constexpr ArenaUser(fstd::allocator_arg_t, Arena allocator, int initial)
        : value(initial), arena(allocator.id)
    {
    }
    constexpr ArenaUser(fstd::allocator_arg_t, Arena allocator, const ArenaUser& other)
        : value(other.value), arena(allocator.id)
    {
    }
};

} // namespace

TEST_CASE("tuple's layout and traits", "[libcxx]")
{
    using Triple = fstd::tuple<int, char, double>;
    STATIC_REQUIRE(fstd::tuple_size_v<Triple> == 3 && fstd::tuple_size_v<const Triple> == 3);
    STATIC_REQUIRE(fstd::is_same_v<fstd::tuple_element_t<1, Triple>, char>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::tuple_element_t<2, const Triple>, const double>);
    STATIC_REQUIRE(fstd::tuple_size_v<fstd::tuple<>> == 0);
    STATIC_REQUIRE(sizeof(fstd::tuple<Empty, int>) == sizeof(int));
    STATIC_REQUIRE(sizeof(fstd::tuple<int, Empty>) == sizeof(int));
    STATIC_REQUIRE(fstd::is_trivially_copy_constructible_v<Triple>);
    STATIC_REQUIRE(fstd::is_trivially_destructible_v<Triple>);
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<fstd::tuple<>>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::tuple<int, MoveOnly>>);
    STATIC_REQUIRE(fstd::is_move_constructible_v<fstd::tuple<int, MoveOnly>>);
    STATIC_REQUIRE(!fstd::is_copy_assignable_v<fstd::tuple<int, MoveOnly>>);
    STATIC_REQUIRE(fstd::is_nothrow_move_assignable_v<fstd::tuple<int, MoveOnly>>);
    STATIC_REQUIRE(!fstd::is_default_constructible_v<fstd::tuple<int&>>);
}

TEST_CASE("tuple constructs implicitly only where its elements do", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_convertible_v<int, fstd::tuple<long>>);
    STATIC_REQUIRE(!fstd::is_convertible_v<int, fstd::tuple<ExplicitOnly>>);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::tuple<ExplicitOnly>, int>);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::tuple<int, int>, fstd::tuple<long, long>>);
    STATIC_REQUIRE(!fstd::is_convertible_v<fstd::tuple<int>, fstd::tuple<ExplicitOnly>>);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::pair<int, char>, fstd::tuple<long, int>>);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::tuple<int, int>, fstd::array<int, 2>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::tuple<int, int>, fstd::array<int, 3>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::tuple<int, int>, int>);

    constexpr fstd::tuple<int, double> defaulted = {};
    STATIC_REQUIRE(fstd::get<0>(defaulted) == 0 && fstd::get<1>(defaulted) == 0.0);
    constexpr fstd::tuple<long, double> converted = fstd::tuple<int, float>(2, 0.5f);
    STATIC_REQUIRE(fstd::get<0>(converted) == 2 && fstd::get<1>(converted) == 0.5);
    constexpr fstd::tuple<int, char> from_pair = fstd::pair<int, char>(1, 'x');
    STATIC_REQUIRE(fstd::get<1>(from_pair) == 'x');
    constexpr fstd::tuple<int, int, int> from_array(fstd::array<int, 3>{4, 5, 6});
    STATIC_REQUIRE(fstd::get<2>(from_array) == 6);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::tuple(1, 'a')), fstd::tuple<int, char>>);
    STATIC_REQUIRE(
        fstd::is_same_v<decltype(fstd::tuple(fstd::pair(1, 2.0))), fstd::tuple<int, double>>);

    // A one-element tuple of tuples copies the inner tuple instead of converting its element.
    fstd::tuple<int> inner(7);
    fstd::tuple<fstd::tuple<int>> nested(inner);
    REQUIRE(fstd::get<0>(fstd::get<0>(nested)) == 7);

    {
        fstd::tuple<Tracked, int> tracked(Tracked(3), 4);
        REQUIRE(Tracked::alive == 1);
        fstd::tuple<Tracked, int> copy = tracked;
        fstd::tuple<Tracked, int> moved = fstd::move(tracked);
        REQUIRE((Tracked::alive == 3 && fstd::get<0>(moved).value == 3));
        REQUIRE(fstd::get<0>(tracked).value == -1);
    }
    REQUIRE(Tracked::alive == 0);

    fstd::tuple<MoveOnly, int> owned(MoveOnly(5), 1);
    fstd::tuple<MoveOnly, int> taken = fstd::move(owned);
    REQUIRE(fstd::get<0>(taken).value == 5);
}

TEST_CASE("pair's piecewise constructor builds each member from a tuple", "[libcxx]")
{
    constexpr fstd::pair<Point, ExplicitOnly> pair(
        fstd::piecewise_construct, fstd::forward_as_tuple(1, 2), fstd::forward_as_tuple(3));
    STATIC_REQUIRE(pair.first.y == 2 && pair.second.value == 3);

    fstd::pair<MoveOnly, fstd::tuple<>> moved(fstd::piecewise_construct,
                                              fstd::forward_as_tuple(MoveOnly(4)),
                                              fstd::tuple<>());
    REQUIRE(moved.first.value == 4);
}

TEST_CASE("tuple's get reads by index and by type", "[libcxx]")
{
    fstd::tuple<int, char, long> values(1, 'b', 3);
    fstd::get<0>(values) = 9;
    fstd::get<long>(values) += 1;
    REQUIRE((fstd::get<int>(values) == 9 && fstd::get<char>(values) == 'b'));
    REQUIRE(fstd::get<2>(values) == 4);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<0>(fstd::move(values))), int&&>);
    const auto& constant = values;
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<1>(constant)), const char&>);

    int target = 0;
    fstd::tuple<int&> reference(target);
    fstd::get<0>(reference) = 5;
    REQUIRE(target == 5);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<0>(fstd::move(reference))), int&>);

}

TEST_CASE("tuple assigns through its elements", "[libcxx]")
{
    fstd::tuple<int, long> target;
    target = fstd::tuple<int, int>(1, 2);
    REQUIRE((fstd::get<0>(target) == 1 && fstd::get<1>(target) == 2));
    target = fstd::pair<short, short>(3, 4);
    REQUIRE(fstd::get<1>(target) == 4);
    target = fstd::array<int, 2>{5, 6};
    REQUIRE(fstd::get<0>(target) == 5);

    int first = 0;
    long second = 0;
    fstd::tie(first, second) = fstd::make_tuple(7, 8L);
    REQUIRE((first == 7 && second == 8));
    fstd::tie(fstd::ignore, second) = fstd::pair<int, long>(1, 9);
    REQUIRE((first == 7 && second == 9));

    fstd::tuple<Tracked> tracked(Tracked(1));
    fstd::tuple<Tracked> other(Tracked(2));
    tracked = other;
    REQUIRE(fstd::get<0>(tracked).value == 2);
    tracked = fstd::tuple<Tracked>(Tracked(3));
    REQUIRE(fstd::get<0>(tracked).value == 3);

    constexpr auto assigned = [] {
        fstd::tuple<int, int> value(1, 2);
        value = fstd::tuple<int, int>(3, 4);
        return fstd::get<0>(value) + fstd::get<1>(value);
    }();
    STATIC_REQUIRE(assigned == 7);
}

TEST_CASE("tuple swaps", "[libcxx]")
{
    fstd::tuple<int, MoveOnly> left(1, MoveOnly(2));
    fstd::tuple<int, MoveOnly> right(3, MoveOnly(4));
    swap(left, right);
    REQUIRE((fstd::get<0>(left) == 3 && fstd::get<1>(right).value == 2));
    left.swap(right);
    REQUIRE(fstd::get<1>(left).value == 2);
    STATIC_REQUIRE(fstd::is_nothrow_swappable_v<fstd::tuple<int, long>>);
    STATIC_REQUIRE(!fstd::is_swappable_v<fstd::tuple<const int>>);
}

TEST_CASE("tuple compares element by element", "[libcxx]")
{
    constexpr fstd::tuple<int, char> small(1, 'a');
    constexpr fstd::tuple<long, int> large(1, 'b');
    STATIC_REQUIRE(small != large && small < large && large > small && small <= small);
    STATIC_REQUIRE(fstd::tuple<>() == fstd::tuple<>() && (fstd::tuple<>() <=> fstd::tuple<>()) == 0);
    STATIC_REQUIRE(fstd::is_same_v<decltype(small <=> large), fstd::strong_ordering>);

    using Weak = fstd::tuple<int, LessOnly>;
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::declval<Weak>() <=> fstd::declval<Weak>()),
                                   fstd::weak_ordering>);
    STATIC_REQUIRE((Weak(1, LessOnly{2}) <=> Weak(1, LessOnly{3})) < 0);

    STATIC_REQUIRE(fstd::equality_comparable<fstd::tuple<int, long>>);
    STATIC_REQUIRE(!fstd::equality_comparable<fstd::tuple<int, NotComparable>>);

    const double nan = __builtin_nan("");
    const auto order = fstd::tuple<int, double>(1, nan) <=> fstd::tuple<int, double>(1, 0.0);
    REQUIRE(order == fstd::partial_ordering::unordered);
    REQUIRE((fstd::tuple<int, double>(0, nan) <=> fstd::tuple<int, double>(1, 0.0)) < 0);
}

TEST_CASE("make_tuple, forward_as_tuple and tie", "[libcxx]")
{
    int value = 1;
    STATIC_REQUIRE(
        fstd::is_same_v<decltype(fstd::make_tuple(1, 'a', 2.0)), fstd::tuple<int, char, double>>);
    STATIC_REQUIRE(
        fstd::is_same_v<decltype(fstd::make_tuple(fstd::ref(value))), fstd::tuple<int&>>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::forward_as_tuple(value, 2)),
                                   fstd::tuple<int&, int&&>>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::tie(value)), fstd::tuple<int&>>);

    auto wrapped = fstd::make_tuple(fstd::ref(value));
    fstd::get<0>(wrapped) = 4;
    REQUIRE(value == 4);
}

TEST_CASE("tuple_cat joins tuples, pairs and arrays", "[libcxx]")
{
    constexpr auto joined = fstd::tuple_cat(fstd::tuple<int, char>(1, 'x'), fstd::pair<long, int>(2, 3),
                                            fstd::array<short, 2>{4, 5}, fstd::tuple<>());
    STATIC_REQUIRE(fstd::is_same_v<fstd::remove_cv_t<decltype(joined)>,
                                   fstd::tuple<int, char, long, int, short, short>>);
    STATIC_REQUIRE(fstd::get<1>(joined) == 'x' && fstd::get<3>(joined) == 3 &&
                   fstd::get<5>(joined) == 5);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::tuple_cat()), fstd::tuple<>>);

    int target = 0;
    auto references = fstd::tuple_cat(fstd::tie(target), fstd::make_tuple(2));
    fstd::get<0>(references) = 6;
    REQUIRE(target == 6);

    auto owned = fstd::tuple_cat(fstd::make_tuple(MoveOnly(7)), fstd::make_tuple(8));
    REQUIRE((fstd::get<0>(owned).value == 7 && fstd::get<1>(owned) == 8));
}

TEST_CASE("apply and make_from_tuple unpack a tuple-like object", "[libcxx]")
{
    constexpr auto sum = [](auto... values) { return (values + ... + 0); };
    STATIC_REQUIRE(fstd::apply(sum, fstd::tuple<int, long>(1, 2)) == 3);
    STATIC_REQUIRE(fstd::apply(sum, fstd::array<int, 3>{1, 2, 3}) == 6);
    STATIC_REQUIRE(fstd::apply(sum, fstd::pair<int, int>(4, 5)) == 9);
    STATIC_REQUIRE(fstd::apply(sum, fstd::tuple<>()) == 0);

    constexpr Point point = fstd::make_from_tuple<Point>(fstd::tuple<int, int>(3, 4));
    STATIC_REQUIRE(point.x == 3 && point.y == 4);
    STATIC_REQUIRE(fstd::make_from_tuple<ExplicitOnly>(fstd::array<int, 1>{2}).value == 2);

    int target = 0;
    fstd::apply([](int& value) { value = 3; }, fstd::tie(target));
    REQUIRE(target == 3);
    auto moved = fstd::apply([](MoveOnly&& value) { return value.value; },
                             fstd::make_tuple(MoveOnly(4)));
    REQUIRE(moved == 4);
}

TEST_CASE("tuple's common type and common reference", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_type_t<fstd::tuple<int, short>, fstd::tuple<long, int>>,
                                   fstd::tuple<long, int>>);
    STATIC_REQUIRE(
        fstd::is_same_v<fstd::common_reference_t<fstd::tuple<int&>&, fstd::tuple<const int&>&>,
                        fstd::tuple<const int&>>);
}

TEST_CASE("tuple's allocator-extended constructors pass the allocator to its elements",
          "[libcxx]")
{
    constexpr Arena arena{7};
    STATIC_REQUIRE(fstd::uses_allocator_v<fstd::tuple<int>, Arena>);

    constexpr fstd::tuple<ArenaUser, int> defaulted(fstd::allocator_arg, arena);
    STATIC_REQUIRE(fstd::get<0>(defaulted).arena == 7);
    constexpr fstd::tuple<ArenaUser, int> from_values(fstd::allocator_arg, arena, 3, 4);
    STATIC_REQUIRE((fstd::get<0>(from_values).value == 3 && fstd::get<0>(from_values).arena == 7 &&
                    fstd::get<1>(from_values) == 4));
    constexpr fstd::tuple<ArenaUser, int> copied(fstd::allocator_arg, Arena{9}, from_values);
    STATIC_REQUIRE((fstd::get<0>(copied).value == 3 && fstd::get<0>(copied).arena == 9));
    constexpr fstd::tuple<ArenaUser, long> from_pair(fstd::allocator_arg, arena,
                                                     fstd::pair<int, int>(5, 6));
    STATIC_REQUIRE((fstd::get<0>(from_pair).value == 5 && fstd::get<0>(from_pair).arena == 7));
    constexpr fstd::tuple<fstd::pair<ArenaUser, int>> nested(fstd::allocator_arg, arena,
                                                             fstd::pair<int, int>(1, 2));
    STATIC_REQUIRE(fstd::get<0>(nested).first.arena == 7);
    constexpr fstd::tuple<> empty(fstd::allocator_arg, arena);
    (void)empty;

    fstd::tuple<MoveOnly> moved(fstd::allocator_arg, arena, fstd::tuple<MoveOnly>(MoveOnly(2)));
    REQUIRE(fstd::get<0>(moved).value == 2);
}
