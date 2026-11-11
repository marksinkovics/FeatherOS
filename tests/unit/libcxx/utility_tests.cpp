#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/compare"
#include "libcxx/include/concepts"
#include "libcxx/include/initializer_list"
#include "libcxx/include/new"
#include "libcxx/include/utility"

#include <cstring>

// The ordering types, std::initializer_list and std::align_val_t are the host's here, since the
// compiler names them; the QEMU program cxxtest checks FeatherOS's own.

namespace
{

namespace fstd = featheros_std;

struct Ordered
{
    int value;
    auto operator<=>(const Ordered&) const = default;
};
// Only <, as older types have; synth-three-way makes a weak ordering of it.
struct LessOnly
{
    int value;
    bool operator<(const LessOnly& other) const { return value < other.value; }
    bool operator==(const LessOnly& other) const { return value == other.value; }
};
struct NotComparable
{
};
struct NotCopyable
{
    NotCopyable() = default;
    NotCopyable(const NotCopyable&) = delete;
    NotCopyable& operator=(const NotCopyable&) = delete;
};
struct Explicit
{
    explicit Explicit(int) {}
};

template <class T>
concept HasThreeWayResult = requires { typename fstd::compare_three_way_result_t<T>; };

namespace Adl
{
struct Counted
{
    int swaps = 0;
};
void swap(Counted& left, Counted& right)
{
    ++left.swaps;
    ++right.swaps;
}
} // namespace Adl

enum class Color : unsigned char
{
    Red = 3
};

} // namespace

TEST_CASE("utility moves, forwards and exchanges", "[libcxx]")
{
    int value = 1;
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::move(value)), int&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::forward<int&>(value)), int&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::forward<int>(value)), int&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::as_const(value)), const int&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::forward_like<const Ordered&>(value)), const int&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::forward_like<Ordered>(value)), int&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::forward_like<const Ordered>(value)), const int&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::move_if_noexcept(value)), int&&>);
    REQUIRE(fstd::exchange(value, 5) == 1);
    REQUIRE(value == 5);
    STATIC_REQUIRE(fstd::to_underlying(Color::Red) == 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::to_underlying(Color::Red)), unsigned char>);
}

TEST_CASE("utility compares integers by value", "[libcxx]")
{
    STATIC_REQUIRE(fstd::cmp_less(-1, 0u) && !fstd::cmp_less(0u, -1));
    STATIC_REQUIRE(!fstd::cmp_equal(-1, static_cast<unsigned>(-1)) && fstd::cmp_equal(5u, 5L));
    STATIC_REQUIRE(fstd::cmp_greater(1u, -1) && fstd::cmp_less_equal(-3, -3L));
    STATIC_REQUIRE(fstd::cmp_greater_equal(0ULL, -1LL) && fstd::cmp_not_equal(-1, 4294967295u));
    STATIC_REQUIRE(fstd::in_range<unsigned char>(255) && !fstd::in_range<unsigned char>(256));
    STATIC_REQUIRE(!fstd::in_range<unsigned>(-1) && fstd::in_range<signed char>(-128));
}

TEST_CASE("utility builds integer sequences", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::make_index_sequence<0>, fstd::index_sequence<>>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::make_integer_sequence<int, 3>,
                                   fstd::integer_sequence<int, 0, 1, 2>>);
    STATIC_REQUIRE(fstd::index_sequence_for<int, char, long>::size() == 3);
}

TEST_CASE("pair constructs, converts and assigns", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::pair{1, 'a'}), fstd::pair<int, char>>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::make_pair(1, "text")),
                                   fstd::pair<int, const char*>>);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::pair<int, int>, fstd::pair<long, long>>);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::pair<Explicit, int>, int, int> &&
                   !fstd::is_convertible_v<fstd::pair<int, int>, fstd::pair<Explicit, int>>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::pair<NotCopyable, int>>);
    STATIC_REQUIRE(!fstd::is_copy_assignable_v<fstd::pair<const int, int>>);
    STATIC_REQUIRE(fstd::is_trivially_copy_constructible_v<fstd::pair<int, int>>);

    constexpr fstd::pair<int, double> defaulted;
    STATIC_REQUIRE(defaulted.first == 0 && defaulted.second == 0.0);

    fstd::pair<int, const char*> numbered(3, "three");
    fstd::pair<long, const char*> widened = numbered;
    REQUIRE(widened.first == 3);
    REQUIRE(std::strcmp(widened.second, "three") == 0);

    int first = 0;
    int second = 0;
    fstd::pair<int&, int&> references(first, second);
    references = fstd::pair<int, int>(4, 5);
    REQUIRE((first == 4 && second == 5));

    fstd::pair<int, int> left(1, 2);
    fstd::pair<int, int> right(3, 4);
    swap(left, right);
    REQUIRE((left.first == 3 && right.second == 2));
}

TEST_CASE("pair is tuple-like", "[libcxx]")
{
    using Pair = fstd::pair<int, char>;
    STATIC_REQUIRE(fstd::tuple_size_v<Pair> == 2 && fstd::tuple_size_v<const Pair> == 2);
    STATIC_REQUIRE(fstd::is_same_v<fstd::tuple_element_t<0, Pair>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::tuple_element_t<1, const Pair>, const char>);

    Pair pair(7, 'x');
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<0>(pair)), int&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<1>(fstd::move(pair))), char&&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<char>(fstd::as_const(pair))), const char&>);
    REQUIRE(fstd::get<0>(pair) == 7);
    REQUIRE(fstd::get<char>(pair) == 'x');
    fstd::get<int>(pair) = 8;
    REQUIRE(pair.first == 8);
}

TEST_CASE("pair compares element by element", "[libcxx]")
{
    STATIC_REQUIRE(fstd::pair{1, 2} == fstd::pair{1, 2L});
    STATIC_REQUIRE(fstd::pair{1, 2} < fstd::pair{1, 3} && fstd::pair{2, 0} > fstd::pair{1, 9});
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::pair{1, 2} <=> fstd::pair{1, 2}),
                                   std::strong_ordering>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::pair{1, 2.0} <=> fstd::pair{1, 2.0}),
                                   std::partial_ordering>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::pair{LessOnly{1}, 2} <=> fstd::pair{LessOnly{1}, 2}),
                                   std::weak_ordering>);
    REQUIRE(fstd::pair{LessOnly{1}, 5} < fstd::pair{LessOnly{2}, 0});
    REQUIRE(fstd::pair{Ordered{1}, 5} >= fstd::pair{Ordered{1}, 5});
}

TEST_CASE("pair has a common type and reference", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_type_t<fstd::pair<int, short>, fstd::pair<long, int>>,
                                   fstd::pair<long, int>>);
    STATIC_REQUIRE(fstd::is_same_v<
                   fstd::common_reference_t<fstd::pair<int&, int&>, fstd::pair<const int&, int&>>,
                   fstd::pair<const int&, int&>>);
}

TEST_CASE("compare finds categories and compares", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_comparison_category_t<>, std::strong_ordering>);
    STATIC_REQUIRE(fstd::is_same_v<
                   fstd::common_comparison_category_t<std::strong_ordering, std::partial_ordering>,
                   std::partial_ordering>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_comparison_category_t<std::weak_ordering, int>, void>);
    STATIC_REQUIRE(fstd::is_lt(std::strong_ordering::less) && fstd::is_eq(std::weak_ordering::equivalent));
    STATIC_REQUIRE(!fstd::is_lteq(std::partial_ordering::unordered) &&
                   fstd::is_neq(std::partial_ordering::unordered));
    STATIC_REQUIRE(fstd::three_way_comparable<int> && fstd::three_way_comparable<Ordered>);
    STATIC_REQUIRE(!fstd::three_way_comparable<double, std::strong_ordering> &&
                   !fstd::three_way_comparable<NotComparable>);
    STATIC_REQUIRE(fstd::three_way_comparable_with<int, long> &&
                   !fstd::three_way_comparable_with<int, Ordered>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::compare_three_way_result_t<Ordered>, std::strong_ordering>);
    STATIC_REQUIRE(!HasThreeWayResult<NotComparable>);
    STATIC_REQUIRE(fstd::compare_three_way()(1, 2) < 0);
}

TEST_CASE("concepts classify types", "[libcxx]")
{
    STATIC_REQUIRE(fstd::same_as<int, int> && !fstd::same_as<int, const int>);
    STATIC_REQUIRE(fstd::convertible_to<int, long> && !fstd::convertible_to<int, Explicit>);
    STATIC_REQUIRE(fstd::integral<char> && fstd::signed_integral<int> &&
                   fstd::unsigned_integral<bool> && !fstd::integral<float>);
    STATIC_REQUIRE(fstd::floating_point<float> && !fstd::floating_point<int>);
    STATIC_REQUIRE(fstd::assignable_from<int&, long> && !fstd::assignable_from<int, int>);
    STATIC_REQUIRE(fstd::default_initializable<int> && !fstd::default_initializable<int&>);
    STATIC_REQUIRE(fstd::move_constructible<int> && !fstd::copy_constructible<NotCopyable>);
    STATIC_REQUIRE(fstd::regular<Ordered> && fstd::semiregular<LessOnly> && !fstd::movable<NotCopyable>);
    STATIC_REQUIRE(fstd::totally_ordered<int> && !fstd::totally_ordered<LessOnly>);
    STATIC_REQUIRE(fstd::equality_comparable_with<int, double> &&
                   !fstd::equality_comparable_with<int, Ordered>);
    STATIC_REQUIRE(fstd::common_with<int, long> && fstd::common_reference_with<int&, long&>);
    STATIC_REQUIRE(fstd::invocable<int (*)(int), short> && fstd::predicate<bool (*)(int), int>);
    STATIC_REQUIRE(fstd::strict_weak_order<bool (*)(int, int), int, long>);
    STATIC_REQUIRE(fstd::swappable<int> && fstd::swappable<Adl::Counted> &&
                   !fstd::swappable<NotCopyable>);
    STATIC_REQUIRE(fstd::swappable_with<int&, int&> && !fstd::swappable_with<int&, long&>);
}

TEST_CASE("ranges::swap uses argument-dependent lookup, arrays and moves", "[libcxx]")
{
    Adl::Counted left;
    Adl::Counted right;
    fstd::ranges::swap(left, right);
    REQUIRE((left.swaps == 1 && right.swaps == 1));

    Adl::Counted lefts[2];
    Adl::Counted rights[2];
    fstd::ranges::swap(lefts, rights);
    REQUIRE((lefts[1].swaps == 1 && rights[0].swaps == 1));

    int first = 1;
    int second = 2;
    fstd::ranges::swap(first, second);
    REQUIRE((first == 2 && second == 1));
}

TEST_CASE("new launders and initializer_list iterates", "[libcxx]")
{
    alignas(int) unsigned char storage[sizeof(int)];
    int* placed = ::new (storage) int(9);
    REQUIRE(fstd::launder(reinterpret_cast<int*>(storage)) == placed);
    STATIC_REQUIRE(fstd::hardware_destructive_interference_size == 64);
    STATIC_REQUIRE(fstd::is_same_v<fstd::align_val_t, std::align_val_t>);

    const fstd::initializer_list<int> list = {1, 2, 3};
    int sum = 0;
    for (const int* element = fstd::begin(list); element != fstd::end(list); ++element)
    {
        sum += *element;
    }
    REQUIRE(sum == 6);
}
