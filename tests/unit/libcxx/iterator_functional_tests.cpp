#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/algorithm"
#include "libcxx/include/functional"
#include "libcxx/include/initializer_list"
#include "libcxx/include/iterator"

#include <deque>
#include <list>
#include <string>
#include <vector>

namespace
{

namespace fstd = featheros_std;

// A forward iterator with the C++17 member types, as hand-written iterators have.
struct ListIterator
{
    using difference_type = fstd::ptrdiff_t;
    using value_type = int;
    using pointer = const int*;
    using reference = const int&;
    using iterator_category = fstd::forward_iterator_tag;

    const int* current = nullptr;

    reference operator*() const { return *current; }
    ListIterator& operator++()
    {
        ++current;
        return *this;
    }
    ListIterator operator++(int)
    {
        ListIterator previous = *this;
        ++current;
        return previous;
    }
    bool operator==(const ListIterator&) const = default;
};

// Only the two types C++20 needs: iterator_traits must work out the rest.
struct BareIterator
{
    using difference_type = fstd::ptrdiff_t;
    using value_type = int;

    int* current = nullptr;

    int& operator*() const { return *current; }
    BareIterator& operator++()
    {
        ++current;
        return *this;
    }
    BareIterator operator++(int)
    {
        BareIterator previous = *this;
        ++current;
        return previous;
    }
    bool operator==(const BareIterator&) const = default;
};

struct Container
{
    int values[3] = {1, 2, 3};
    int* begin() { return values; }
    int* end() { return values + 3; }
    const int* begin() const { return values; }
    const int* end() const { return values + 3; }
    fstd::size_t size() const { return 3; }
};

namespace Adl
{
struct FreeRange
{
    int values[2] = {7, 8};
};
int* begin(FreeRange& range)
{
    return range.values;
}
int* end(FreeRange& range)
{
    return range.values + 2;
}
} // namespace Adl

struct Point
{
    int x;
    int Twice() const { return x * 2; }
};

template <class T>
concept Transparent = requires { typename T::is_transparent; };

int AddOne(int value)
{
    return value + 1;
}

} // namespace

TEST_CASE("iterator traits and concepts classify iterators", "[libcxx]")
{
    STATIC_REQUIRE(fstd::contiguous_iterator<int*> && fstd::contiguous_iterator<const int*>);
    STATIC_REQUIRE(!fstd::input_iterator<void*>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iter_value_t<const int*>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iter_reference_t<const int*>, const int&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iter_rvalue_reference_t<int*>, int&&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iterator_traits<int*>::iterator_category,
                                   fstd::random_access_iterator_tag>);

    STATIC_REQUIRE(fstd::forward_iterator<ListIterator>);
    STATIC_REQUIRE(!fstd::bidirectional_iterator<ListIterator>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iterator_traits<ListIterator>::pointer, const int*>);

    STATIC_REQUIRE(fstd::is_same_v<fstd::iterator_traits<BareIterator>::value_type, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iterator_traits<BareIterator>::iterator_category,
                                   fstd::forward_iterator_tag>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::iterator_traits<BareIterator>::difference_type,
                                   fstd::ptrdiff_t>);
    STATIC_REQUIRE(fstd::forward_iterator<BareIterator>);

    STATIC_REQUIRE(fstd::random_access_iterator<fstd::reverse_iterator<int*>>);
    STATIC_REQUIRE(!fstd::contiguous_iterator<fstd::reverse_iterator<int*>>);
    STATIC_REQUIRE(fstd::sized_sentinel_for<int*, int*>);
}

TEST_CASE("iterator operations and reverse_iterator walk a range", "[libcxx]")
{
    int values[] = {1, 2, 3, 4};
    REQUIRE(fstd::distance(values, values + 4) == 4);
    REQUIRE(*fstd::next(values, 2) == 3);
    REQUIRE(*fstd::prev(values + 4) == 4);

    const int list_values[] = {5, 6, 7};
    ListIterator list{list_values};
    fstd::advance(list, 2);
    REQUIRE(*list == 7);
    REQUIRE(fstd::distance(ListIterator{list_values}, ListIterator{list_values + 3}) == 3);

    auto reversed = fstd::rbegin(values);
    REQUIRE(*reversed == 4);
    REQUIRE(reversed[1] == 3);
    REQUIRE(*(reversed + 3) == 1);
    REQUIRE(fstd::rend(values) - reversed == 4);
    REQUIRE(reversed < fstd::rend(values));
    REQUIRE((fstd::rbegin(values) <=> fstd::rend(values)) < 0);
    REQUIRE(fstd::make_reverse_iterator(values + 1).base() == values + 1);

    int sum = 0;
    for (auto it = fstd::rbegin(values); it != fstd::rend(values); ++it)
    {
        sum = sum * 10 + *it;
    }
    REQUIRE(sum == 4321);
}

TEST_CASE("range access finds members, free functions and arrays", "[libcxx]")
{
    Container container;
    REQUIRE(fstd::ranges::begin(container) == container.values);
    REQUIRE(fstd::ranges::size(container) == 3);
    REQUIRE(fstd::ranges::data(container) == container.values);
    REQUIRE(!fstd::ranges::empty(container));
    REQUIRE(fstd::ranges::ssize(container) == 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::ranges::cbegin(container)), const int*>);

    Adl::FreeRange free_range;
    REQUIRE(*fstd::ranges::begin(free_range) == 7);
    REQUIRE(fstd::ranges::size(free_range) == 2);
    STATIC_REQUIRE(fstd::ranges::contiguous_range<Adl::FreeRange>);
    STATIC_REQUIRE(!fstd::ranges::range<const Adl::FreeRange>);

    int array[5] = {};
    REQUIRE(fstd::ranges::end(array) == array + 5);
    STATIC_REQUIRE(fstd::ranges::size(array) == 5);
    STATIC_REQUIRE(fstd::size(array) == 5 && fstd::ssize(array) == 5);
    REQUIRE(fstd::data(array) == array);
    REQUIRE(!fstd::empty(array));
    REQUIRE(fstd::size(container) == 3);

    STATIC_REQUIRE(fstd::ranges::contiguous_range<Container>);
    STATIC_REQUIRE(fstd::ranges::sized_range<Container>);
    STATIC_REQUIRE(fstd::ranges::borrowed_range<Container&> &&
                   !fstd::ranges::borrowed_range<Container>);
    // An rvalue that is not a borrowed range would dangle.
    STATIC_REQUIRE(!fstd::is_invocable_v<decltype(fstd::ranges::begin), Container>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::ranges::range_value_t<Container>, int>);
}

TEST_CASE("invoke and reference_wrapper call anything callable", "[libcxx]")
{
    Point point{4};
    STATIC_REQUIRE(fstd::invoke([](int value) { return value * 3; }, 2) == 6);
    REQUIRE(fstd::invoke(&Point::Twice, point) == 8);
    REQUIRE(fstd::invoke(&Point::Twice, &point) == 8);
    REQUIRE(fstd::invoke(&Point::x, point) == 4);
    REQUIRE(fstd::invoke(AddOne, 1) == 2);
    REQUIRE(fstd::invoke_r<long>(AddOne, 1) == 2L);
    fstd::invoke_r<void>(AddOne, 1);

    int value = 1;
    auto reference = fstd::ref(value);
    reference.get() = 5;
    REQUIRE(value == 5);
    int& unwrapped = reference;
    REQUIRE(&unwrapped == &value);
    REQUIRE(fstd::invoke(&Point::x, fstd::ref(point)) == 4);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::cref(value)), fstd::reference_wrapper<const int>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::reference_wrapper<int>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::unwrap_ref_decay_t<fstd::reference_wrapper<int>>, int&>);

    auto function = fstd::ref(AddOne);
    REQUIRE(function(2) == 3);
}

TEST_CASE("function objects compare and compute", "[libcxx]")
{
    STATIC_REQUIRE(fstd::plus<int>{}(2, 3) == 5 && fstd::minus<>{}(5, 2.5) == 2.5);
    STATIC_REQUIRE(fstd::multiplies<>{}(3, 4) == 12 && fstd::modulus<int>{}(7, 3) == 1);
    STATIC_REQUIRE(fstd::negate<>{}(4) == -4 && fstd::logical_not<bool>{}(false));
    STATIC_REQUIRE(fstd::bit_xor<>{}(0b1100, 0b1010) == 0b0110 && fstd::bit_not<unsigned char>{}(0) == 0xFF);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::plus<short>{}(1, 2)), short>);
    STATIC_REQUIRE(fstd::less<>{}(1, 2L) && fstd::greater_equal<int>{}(2, 2));
    STATIC_REQUIRE(Transparent<fstd::less<>> && !Transparent<fstd::less<int>>);
    STATIC_REQUIRE(fstd::ranges::less{}(1, 2) && !fstd::ranges::greater{}(1, 2));
    STATIC_REQUIRE(fstd::ranges::equal_to{}(3, 3L) && fstd::ranges::less_equal{}(3, 3));
    STATIC_REQUIRE(!fstd::is_invocable_v<fstd::ranges::less, int, Point>);

    int value = 0;
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::identity{}(value)), int&>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::identity{}(1)), int&&>);
}

TEST_CASE("insert iterators append, prepend and insert", "[libcxx]")
{
    std::vector<int> back;
    auto appender = fstd::back_inserter(back);
    *appender = 1;
    ++appender;
    *appender++ = 2;
    REQUIRE(back == std::vector<int>{1, 2});
    STATIC_REQUIRE(fstd::output_iterator<fstd::back_insert_iterator<std::vector<int>>, int>);

    std::deque<int> front;
    auto prepender = fstd::front_inserter(front);
    *prepender = 1;
    *prepender = 2;
    REQUIRE(front == std::deque<int>{2, 1});

    std::list<int> middle{1, 4};
    auto inserter = fstd::inserter(middle, std::next(middle.begin()));
    *inserter = 2;
    *inserter = 3;
    REQUIRE(middle == std::list<int>{1, 2, 3, 4});

    std::vector<std::string> strings;
    std::string moved = "moved into the vector";
    fstd::back_inserter(strings) = std::move(moved);
    REQUIRE((strings.front() == "moved into the vector" && moved.empty()));
}

TEST_CASE("move_iterator moves what it reads", "[libcxx]")
{
    std::vector<std::string> source{"one", "two"};
    // The host's vector does not take our iterators, whose tags are not the host's.
    std::vector<std::string> target;
    fstd::copy(fstd::make_move_iterator(source.begin()), fstd::make_move_iterator(source.end()),
               fstd::back_inserter(target));
    REQUIRE((target[1] == "two" && source[1].empty()));

    using Moving = fstd::move_iterator<int*>;
    STATIC_REQUIRE(fstd::is_same_v<Moving::reference, int&&>);
    STATIC_REQUIRE(fstd::is_same_v<Moving::iterator_concept, fstd::random_access_iterator_tag>);
    STATIC_REQUIRE(fstd::is_same_v<Moving::iterator_category, fstd::random_access_iterator_tag>);
    STATIC_REQUIRE(fstd::random_access_iterator<Moving>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::move_iterator<ListIterator>::iterator_concept,
                                   fstd::forward_iterator_tag>);

    int values[4] = {1, 2, 3, 4};
    Moving first(values);
    Moving last(values + 4);
    REQUIRE((last - first == 4 && first < last && first[2] == 3 && *(first + 1) == 2));
    REQUIRE(((2 + first).base() == values + 2 && (last - 1).base() == values + 3));
    REQUIRE((first <=> last) < 0);
    fstd::move_iterator<const int*> constant = first;
    REQUIRE(constant.base() == values);

    fstd::move_sentinel<int*> end(values + 4);
    REQUIRE((first != end && fstd::next(first, 4) == end && end - first == 4));
    REQUIRE(fstd::sized_sentinel_for<fstd::move_sentinel<int*>, Moving>);
}

TEST_CASE("default_sentinel and unreachable_sentinel", "[libcxx]")
{
    int value = 0;
    STATIC_REQUIRE(fstd::sentinel_for<fstd::unreachable_sentinel_t, int*>);
    REQUIRE(!(&value == fstd::unreachable_sentinel));
    REQUIRE(&value != fstd::unreachable_sentinel);
    STATIC_REQUIRE(fstd::is_empty_v<fstd::default_sentinel_t>);
}
