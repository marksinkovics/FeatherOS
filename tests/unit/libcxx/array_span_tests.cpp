#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/array"
#include "libcxx/include/span"

// Structured bindings ask the host's std::tuple_size, so the QEMU program cxxtest checks them.

namespace
{

namespace fstd = featheros_std;

struct Base
{
    int value;
};
struct Derived : Base
{
};

struct Owner
{
    int values[2] = {3, 4};
    int* data() { return values; }
    fstd::size_t size() const { return 2; }
    int* begin() { return values; }
    int* end() { return values + 2; }
};

constexpr int Sum(fstd::span<const int> values)
{
    int sum = 0;
    for (int value : values)
    {
        sum += value;
    }
    return sum;
}

} // namespace

TEST_CASE("array is an aggregate with container members", "[libcxx]")
{
    constexpr fstd::array<int, 4> numbers{4, 1, 3, 2};
    STATIC_REQUIRE(numbers.size() == 4 && numbers.front() == 4 && numbers.back() == 2);
    STATIC_REQUIRE(numbers[2] == 3 && numbers.at(1) == 1 && *numbers.data() == 4);
    STATIC_REQUIRE(*numbers.rbegin() == 2 && numbers.end() - numbers.begin() == 4);
    STATIC_REQUIRE(fstd::is_aggregate_v<fstd::array<int, 4>>);
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<fstd::array<int, 4>>);
    STATIC_REQUIRE(sizeof(fstd::array<char, 3>) == 3);

    constexpr fstd::array<int, 0> none{};
    STATIC_REQUIRE(none.empty() && none.begin() == none.end() && none.data() == nullptr);

    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::array{1, 2, 3}), fstd::array<int, 3>>);
    constexpr auto copied = fstd::to_array("abc");
    STATIC_REQUIRE(fstd::is_same_v<decltype(copied), const fstd::array<char, 4>>);
    STATIC_REQUIRE(copied[3] == '\0');

    STATIC_REQUIRE(fstd::tuple_size_v<fstd::array<int, 4>> == 4);
    STATIC_REQUIRE(fstd::is_same_v<fstd::tuple_element_t<1, const fstd::array<int, 4>>, const int>);
    STATIC_REQUIRE(fstd::get<0>(numbers) == 4);

    fstd::array<int, 3> filled{};
    filled.fill(7);
    REQUIRE((filled[0] == 7 && filled[2] == 7));
    fstd::array<int, 3> other{1, 2, 3};
    swap(filled, other);
    REQUIRE((filled[0] == 1 && other[1] == 7));
    fstd::get<1>(filled) = 9;
    REQUIRE(filled[1] == 9);
}

TEST_CASE("array compares element by element", "[libcxx]")
{
    constexpr fstd::array<int, 3> small{1, 2, 3};
    constexpr fstd::array<int, 3> large{1, 3, 0};
    STATIC_REQUIRE(small == small && small != large);
    STATIC_REQUIRE(small < large && large >= small);
    STATIC_REQUIRE(fstd::is_same_v<decltype(small <=> large), fstd::strong_ordering>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::array<double, 1>{} <=> fstd::array<double, 1>{}),
                                   fstd::partial_ordering>);
}

TEST_CASE("span views arrays, containers and ranges", "[libcxx]")
{
    int values[] = {1, 2, 3, 4, 5};
    fstd::span<int> all(values);
    REQUIRE(all.size() == 5);
    REQUIRE(all.size_bytes() == 5 * sizeof(int));
    REQUIRE(all.front() == 1);
    REQUIRE(all.back() == 5);
    REQUIRE(all[2] == 3);
    REQUIRE(all.at(4) == 5);

    fstd::span fixed(values);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fixed), fstd::span<int, 5>>);
    STATIC_REQUIRE(sizeof(fstd::span<int, 5>) == sizeof(int*));
    STATIC_REQUIRE(sizeof(fstd::span<int>) == sizeof(int*) + sizeof(fstd::size_t));

    fstd::array<int, 3> array{7, 8, 9};
    fstd::span<const int, 3> from_array(array);
    REQUIRE(from_array[1] == 8);
    fstd::span<int> from_iterators(array.begin(), array.end());
    REQUIRE(from_iterators.size() == 3);
    fstd::span<int> from_count(values + 1, 2);
    REQUIRE(from_count[0] == 2);

    Owner owner;
    fstd::span<int> from_range(owner);
    REQUIRE((from_range.size() == 2 && from_range[1] == 4));
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::span<const int>, Owner>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::span<int>, Owner>);

    STATIC_REQUIRE(fstd::is_convertible_v<fstd::span<int>, fstd::span<const int>>);
    STATIC_REQUIRE(!fstd::is_convertible_v<fstd::span<const int>, fstd::span<int>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::span<Base>, fstd::span<Derived>>);
    STATIC_REQUIRE(!fstd::is_convertible_v<fstd::span<int>, fstd::span<int, 3>>);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::span<int, 3>, fstd::span<int>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::span<int, 4>, int (&)[5]>);
    STATIC_REQUIRE(!fstd::is_default_constructible_v<fstd::span<int, 2>>);

    REQUIRE(Sum(values) == 15);
    REQUIRE(Sum(array) == 24);
    static constexpr int constants[] = {1, 2};
    STATIC_REQUIRE(Sum(constants) == 3);

    STATIC_REQUIRE(fstd::ranges::contiguous_range<fstd::span<int>>);
    STATIC_REQUIRE(fstd::ranges::borrowed_range<fstd::span<int>>);
    STATIC_REQUIRE(fstd::ranges::view<fstd::span<int>>);
}

TEST_CASE("span cuts subspans", "[libcxx]")
{
    int values[] = {1, 2, 3, 4, 5};
    fstd::span<int, 5> all(values);

    auto first = all.first<2>();
    STATIC_REQUIRE(fstd::is_same_v<decltype(first), fstd::span<int, 2>>);
    REQUIRE(first[1] == 2);
    auto last = all.last<2>();
    REQUIRE(last[0] == 4);
    auto middle = all.subspan<1, 3>();
    STATIC_REQUIRE(decltype(middle)::extent == 3);
    REQUIRE((middle.front() == 2 && middle.back() == 4));
    auto tail = all.subspan<2>();
    STATIC_REQUIRE(decltype(tail)::extent == 3);
    REQUIRE(tail[0] == 3);

    fstd::span<int> dynamic = all;
    auto dynamic_tail = dynamic.subspan<3>();
    STATIC_REQUIRE(decltype(dynamic_tail)::extent == fstd::dynamic_extent);
    REQUIRE(dynamic_tail.size() == 2);
    REQUIRE(dynamic.first(3).size() == 3);
    REQUIRE(dynamic.last(1)[0] == 5);
    REQUIRE(dynamic.subspan(1, 2)[1] == 3);
    REQUIRE(dynamic.subspan(4).size() == 1);
    REQUIRE(dynamic.subspan(5).empty());

    int reversed = 0;
    for (auto it = dynamic.rbegin(); it != dynamic.rend(); ++it)
    {
        reversed = reversed * 10 + *it;
    }
    REQUIRE(reversed == 54321);

    unsigned int word = 0x01020304;
    auto bytes = fstd::as_bytes(fstd::span<unsigned int, 1>(&word, 1));
    STATIC_REQUIRE(decltype(bytes)::extent == sizeof(unsigned int));
    REQUIRE(bytes.size() == sizeof(unsigned int));
    auto writable = fstd::as_writable_bytes(fstd::span<unsigned int>(&word, 1));
    writable[0] = fstd::byte{0};
    REQUIRE((word & 0xFF) == 0);
}
