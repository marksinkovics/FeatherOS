#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/algorithm"
#include "libcxx/include/array"
#include "libcxx/include/iterator"

#include <algorithm>
#include <forward_list>
#include <list>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

namespace
{

namespace fstd = featheros_std;

// The key orders; the tag tells equal keys apart, to check stability.
struct Keyed
{
    int key;
    int tag;
    friend bool operator==(const Keyed&, const Keyed&) = default;
};

constexpr bool ByKey(const Keyed& left, const Keyed& right)
{
    return left.key < right.key;
}

// Movable but not copyable, so the algorithms that only move are checked to do so.
struct MoveOnly
{
    int value;
    explicit MoveOnly(int initial) : value(initial) {}
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    friend bool operator<(const MoveOnly& left, const MoveOnly& right)
    {
        return left.value < right.value;
    }
};

// A single-pass iterator, so the algorithms take their input-iterator paths.
struct InputOnly
{
    using iterator_category = fstd::input_iterator_tag;
    using value_type = int;
    using difference_type = ptrdiff_t;
    using pointer = const int*;
    using reference = const int&;
    const int* position;
    const int& operator*() const { return *position; }
    InputOnly& operator++()
    {
        ++position;
        return *this;
    }
    InputOnly operator++(int)
    {
        InputOnly previous = *this;
        ++position;
        return previous;
    }
    friend bool operator==(InputOnly, InputOnly) = default;
};

std::vector<int> RandomValues(size_t count, int range, unsigned seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> distribution(0, range);
    std::vector<int> values(count);
    for (int& value : values)
    {
        value = distribution(generator);
    }
    return values;
}

constexpr fstd::array<int, 6> SortedInConstantEvaluation()
{
    fstd::array<int, 6> values{5, 2, 9, 1, 5, 6};
    fstd::sort(values.begin(), values.end());
    return values;
}

} // namespace

TEST_CASE("the non-modifying algorithms search and count", "[libcxx]")
{
    constexpr fstd::array<int, 8> values{1, 2, 3, 2, 3, 4, 2, 3};
    STATIC_REQUIRE(fstd::all_of(values.begin(), values.end(), [](int value) { return value > 0; }));
    STATIC_REQUIRE(fstd::any_of(values.begin(), values.end(), [](int value) { return value == 4; }));
    STATIC_REQUIRE(fstd::none_of(values.begin(), values.end(), [](int value) { return value > 4; }));
    STATIC_REQUIRE(*fstd::find(values.begin(), values.end(), 3) == 3);
    STATIC_REQUIRE(fstd::find(values.begin(), values.end(), 7) == values.end());
    STATIC_REQUIRE(fstd::find_if_not(values.begin(), values.end(), [](int value) { return value < 3; }) ==
                   values.begin() + 2);
    STATIC_REQUIRE(fstd::count(values.begin(), values.end(), 2) == 3);
    STATIC_REQUIRE(fstd::count_if(values.begin(), values.end(), [](int value) { return value % 2; }) == 4);

    constexpr fstd::array<int, 2> pattern{2, 3};
    STATIC_REQUIRE(fstd::search(values.begin(), values.end(), pattern.begin(), pattern.end()) ==
                   values.begin() + 1);
    STATIC_REQUIRE(fstd::find_end(values.begin(), values.end(), pattern.begin(), pattern.end()) ==
                   values.begin() + 6);
    STATIC_REQUIRE(fstd::find_first_of(values.begin(), values.end(), pattern.rbegin(),
                                       pattern.rend()) == values.begin() + 1);
    STATIC_REQUIRE(fstd::adjacent_find(values.begin(), values.end()) == values.end());
    constexpr fstd::array<int, 5> runs{1, 7, 7, 7, 2};
    STATIC_REQUIRE(fstd::adjacent_find(runs.begin(), runs.end()) == runs.begin() + 1);
    STATIC_REQUIRE(fstd::search_n(runs.begin(), runs.end(), 3, 7) == runs.begin() + 1);
    STATIC_REQUIRE(fstd::search_n(runs.begin(), runs.end(), 4, 7) == runs.end());

    STATIC_REQUIRE(fstd::equal(pattern.begin(), pattern.end(), values.begin() + 1));
    STATIC_REQUIRE(!fstd::equal(pattern.begin(), pattern.end(), values.begin(), values.begin() + 3));
    STATIC_REQUIRE(fstd::mismatch(values.begin(), values.end(), runs.begin(), runs.end()).first ==
                   values.begin() + 1);

    constexpr fstd::array<int, 8> shuffled{3, 2, 4, 3, 2, 1, 3, 2};
    STATIC_REQUIRE(fstd::is_permutation(values.begin(), values.end(), shuffled.begin()));
    STATIC_REQUIRE(!fstd::is_permutation(values.begin(), values.end(), runs.begin(), runs.end()));

    // Forward-only iterators take the generic paths.
    std::forward_list<int> list{4, 5, 6, 5};
    REQUIRE(fstd::count(list.begin(), list.end(), 5) == 2);
    REQUIRE(fstd::equal(list.begin(), list.end(), list.begin(), list.end()));
    int sum = 0;
    REQUIRE(fstd::for_each_n(list.begin(), 2, [&sum](int value) { sum += value; }) ==
            std::next(list.begin(), 2));
    REQUIRE(sum == 9);
}

TEST_CASE("the copying and filling algorithms", "[libcxx]")
{
    int source[5] = {1, 2, 3, 4, 5};
    int target[5] = {};
    REQUIRE(fstd::copy(source, source + 5, target) == target + 5);
    REQUIRE(std::equal(source, source + 5, target));
    // Overlapping copies, which go through memmove for trivially copyable types.
    fstd::copy(source + 1, source + 5, source);
    REQUIRE((source[0] == 2 && source[3] == 5));
    fstd::copy_backward(source, source + 4, source + 5);
    REQUIRE((source[1] == 2 && source[4] == 5));

    std::list<int> list{1, 2, 3};
    std::vector<int> collected;
    fstd::copy_if(list.begin(), list.end(), fstd::back_inserter(collected),
                  [](int value) { return value != 2; });
    REQUIRE(collected == std::vector<int>{1, 3});
    fstd::copy_n(list.begin(), 2, fstd::back_inserter(collected));
    REQUIRE(collected == std::vector<int>{1, 3, 1, 2});

    std::vector<MoveOnly> movable;
    movable.emplace_back(1);
    movable.emplace_back(2);
    std::vector<MoveOnly> moved;
    fstd::move(movable.begin(), movable.end(), fstd::back_inserter(moved));
    REQUIRE((moved.size() == 2 && moved[1].value == 2));
    fstd::move_backward(moved.begin(), moved.begin() + 1, moved.end());
    REQUIRE(moved[1].value == 1);

    int filled[4] = {};
    fstd::fill(filled, filled + 4, 7);
    REQUIRE(fstd::fill_n(filled, 2, 3) == filled + 2);
    REQUIRE((filled[1] == 3 && filled[2] == 7));
    fstd::transform(filled, filled + 4, filled, [](int value) { return value * 2; });
    REQUIRE(filled[3] == 14);
    int sums[4] = {};
    fstd::transform(filled, filled + 4, source, sums, [](int left, int right) { return left + right; });
    REQUIRE(sums[0] == 8);
    int counter = 0;
    fstd::generate(filled, filled + 4, [&counter] { return counter++; });
    REQUIRE((filled[0] == 0 && filled[3] == 3));
    REQUIRE(fstd::generate_n(filled, 2, [] { return 9; }) == filled + 2);
}

TEST_CASE("remove, replace and unique close the gaps they leave", "[libcxx]")
{
    std::vector<int> values{1, 2, 3, 2, 4, 2};
    values.erase(fstd::remove(values.begin(), values.end(), 2), values.end());
    REQUIRE(values == std::vector<int>{1, 3, 4});
    values.erase(fstd::remove_if(values.begin(), values.end(), [](int value) { return value > 3; }),
                 values.end());
    REQUIRE(values == std::vector<int>{1, 3});

    std::vector<int> copied;
    const int source[] = {5, 6, 5, 7};
    fstd::remove_copy(source, source + 4, fstd::back_inserter(copied), 5);
    REQUIRE(copied == std::vector<int>{6, 7});
    fstd::replace_copy(source, source + 4, fstd::back_inserter(copied), 5, 0);
    REQUIRE(copied == std::vector<int>{6, 7, 0, 6, 0, 7});
    fstd::replace(copied.begin(), copied.end(), 6, 1);
    fstd::replace_if(copied.begin(), copied.end(), [](int value) { return value == 0; }, 2);
    REQUIRE(copied == std::vector<int>{1, 7, 2, 1, 2, 7});

    std::vector<int> runs{1, 1, 2, 2, 2, 3, 1, 1};
    runs.erase(fstd::unique(runs.begin(), runs.end()), runs.end());
    REQUIRE(runs == std::vector<int>{1, 2, 3, 1});

    // unique_copy from an input iterator keeps the last value aside.
    std::vector<int> unique;
    const int repeated[] = {4, 4, 5, 5, 4};
    fstd::unique_copy(InputOnly{repeated}, InputOnly{repeated + 5}, fstd::back_inserter(unique));
    REQUIRE(unique == std::vector<int>{4, 5, 4});
}

TEST_CASE("reverse, rotate and shift move elements in place", "[libcxx]")
{
    for (size_t length = 0; length != 9; ++length)
    {
        for (size_t middle = 0; middle <= length; ++middle)
        {
            std::vector<int> ours(length);
            std::iota(ours.begin(), ours.end(), 0);
            std::vector<int> expected = ours;
            std::forward_list<int> forward(ours.begin(), ours.end());
            const auto result = fstd::rotate(ours.begin(), ours.begin() + middle, ours.end());
            std::rotate(expected.begin(), expected.begin() + middle, expected.end());
            REQUIRE(ours == expected);
            REQUIRE(result == ours.begin() + (length - middle));
            fstd::rotate(forward.begin(), std::next(forward.begin(), middle), forward.end());
            REQUIRE(std::equal(forward.begin(), forward.end(), expected.begin(), expected.end()));

            std::vector<int> reversed(length);
            std::iota(reversed.begin(), reversed.end(), 0);
            std::list<int> list(reversed.begin(), reversed.end());
            fstd::reverse(reversed.begin(), reversed.end());
            fstd::reverse(list.begin(), list.end());
            REQUIRE(std::is_sorted(reversed.rbegin(), reversed.rend()));
            REQUIRE(std::equal(list.begin(), list.end(), reversed.begin(), reversed.end()));
        }
    }

    std::vector<int> values{1, 2, 3, 4, 5};
    std::vector<int> rotated;
    fstd::rotate_copy(values.begin(), values.begin() + 2, values.end(), fstd::back_inserter(rotated));
    REQUIRE(rotated == std::vector<int>{3, 4, 5, 1, 2});
    std::vector<int> reversed;
    fstd::reverse_copy(values.begin(), values.end(), fstd::back_inserter(reversed));
    REQUIRE(reversed == std::vector<int>{5, 4, 3, 2, 1});

    REQUIRE(fstd::shift_left(values.begin(), values.end(), 2) == values.begin() + 3);
    REQUIRE((values[0] == 3 && values[2] == 5));
    std::vector<int> right{1, 2, 3, 4, 5};
    REQUIRE(fstd::shift_right(right.begin(), right.end(), 2) == right.begin() + 2);
    REQUIRE((right[2] == 1 && right[4] == 3));
    REQUIRE(fstd::shift_right(right.begin(), right.end(), 9) == right.end());
    std::forward_list<int> forward{1, 2, 3, 4};
    REQUIRE(fstd::shift_right(forward.begin(), forward.end(), 1) == std::next(forward.begin()));
    REQUIRE(*std::next(forward.begin(), 3) == 3);

    int left[3] = {1, 2, 3};
    int other[3] = {4, 5, 6};
    fstd::swap_ranges(left, left + 3, other);
    REQUIRE((left[0] == 4 && other[2] == 3));
}

TEST_CASE("sort, partial_sort and nth_element agree with the host's library", "[libcxx]")
{
    STATIC_REQUIRE(SortedInConstantEvaluation() == fstd::array<int, 6>{1, 2, 5, 5, 6, 9});

    for (size_t length : {0U, 1U, 2U, 3U, 15U, 16U, 17U, 100U, 1000U, 5000U})
    {
        for (int range : {3, 1000000})
        {
            const auto original = RandomValues(length, range, static_cast<unsigned>(length + range));
            auto expected = original;
            std::sort(expected.begin(), expected.end());

            auto sorted = original;
            fstd::sort(sorted.begin(), sorted.end());
            REQUIRE(sorted == expected);
            auto descending = original;
            fstd::sort(descending.begin(), descending.end(), [](int left, int right) { return left > right; });
            REQUIRE(std::equal(descending.rbegin(), descending.rend(), expected.begin()));

            if (length == 0)
            {
                continue;
            }
            const size_t nth = length / 3;
            auto selected = original;
            fstd::nth_element(selected.begin(), selected.begin() + nth, selected.end());
            REQUIRE(selected[nth] == expected[nth]);
            REQUIRE(std::all_of(selected.begin(), selected.begin() + nth,
                                [&](int value) { return value <= selected[nth]; }));
            REQUIRE(std::all_of(selected.begin() + nth, selected.end(),
                                [&](int value) { return value >= selected[nth]; }));

            auto partial = original;
            fstd::partial_sort(partial.begin(), partial.begin() + nth, partial.end());
            REQUIRE(std::equal(partial.begin(), partial.begin() + nth, expected.begin()));
            std::vector<int> smallest(nth);
            REQUIRE(fstd::partial_sort_copy(original.begin(), original.end(), smallest.begin(),
                                            smallest.end()) == smallest.end());
            REQUIRE(std::equal(smallest.begin(), smallest.end(), expected.begin()));
        }
    }

    // Already sorted, reversed and all-equal input, the quicksort's bad cases.
    std::vector<int> ascending(3000);
    std::iota(ascending.begin(), ascending.end(), 0);
    auto backwards = ascending;
    std::reverse(backwards.begin(), backwards.end());
    std::vector<int> same(3000, 4);
    for (auto* values : {&ascending, &backwards, &same})
    {
        fstd::sort(values->begin(), values->end());
        REQUIRE(std::is_sorted(values->begin(), values->end()));
    }

    std::vector<MoveOnly> movable;
    for (int value : {3, 1, 2})
    {
        movable.emplace_back(value);
    }
    fstd::sort(movable.begin(), movable.end());
    REQUIRE((movable[0].value == 1 && movable[2].value == 3));

    std::vector<int> unsorted{1, 2, 4, 3};
    REQUIRE(fstd::is_sorted_until(unsorted.begin(), unsorted.end()) == unsorted.begin() + 3);
    REQUIRE(!fstd::is_sorted(unsorted.begin(), unsorted.end()));
}

TEST_CASE("stable_sort and stable_partition keep equal elements in order", "[libcxx]")
{
    for (size_t length : {0U, 1U, 5U, 16U, 17U, 33U, 1000U, 4097U})
    {
        const auto keys = RandomValues(length, 20, static_cast<unsigned>(length));
        std::vector<Keyed> original;
        for (size_t index = 0; index != length; ++index)
        {
            original.push_back(Keyed{keys[index], static_cast<int>(index)});
        }
        auto expected = original;
        std::stable_sort(expected.begin(), expected.end(), ByKey);
        auto sorted = original;
        fstd::stable_sort(sorted.begin(), sorted.end(), ByKey);
        REQUIRE(sorted == expected);

        auto partitioned = original;
        auto expected_partition = original;
        auto even = [](const Keyed& value) { return value.key % 2 == 0; };
        const auto point = fstd::stable_partition(partitioned.begin(), partitioned.end(), even);
        std::stable_partition(expected_partition.begin(), expected_partition.end(), even);
        REQUIRE(partitioned == expected_partition);
        REQUIRE(fstd::is_partitioned(partitioned.begin(), partitioned.end(), even));
        REQUIRE(fstd::partition_point(partitioned.begin(), partitioned.end(), even) == point);

        // The fallbacks taken when no buffer can be allocated.
        auto in_place = original;
        if (length != 0)
        {
            const auto in_place_point = fstd::__detail::StablePartitionInPlace(
                in_place.begin(), in_place.end(), even, static_cast<ptrdiff_t>(length));
            REQUIRE(in_place == expected_partition);
            REQUIRE(in_place_point - in_place.begin() == point - partitioned.begin());
        }
        auto merged = original;
        const auto half = static_cast<ptrdiff_t>(length / 2);
        std::stable_sort(merged.begin(), merged.begin() + half, ByKey);
        std::stable_sort(merged.begin() + half, merged.end(), ByKey);
        auto compare = ByKey;
        fstd::__detail::MergeInPlace(merged.begin(), merged.begin() + half, merged.end(), half,
                                     static_cast<ptrdiff_t>(length) - half, compare);
        auto expected_merge = original;
        std::stable_sort(expected_merge.begin(), expected_merge.begin() + half, ByKey);
        std::stable_sort(expected_merge.begin() + half, expected_merge.end(), ByKey);
        std::inplace_merge(expected_merge.begin(), expected_merge.begin() + half,
                           expected_merge.end(), ByKey);
        REQUIRE(merged == expected_merge);
    }

    std::list<int> list{1, 2, 3, 4, 5, 6};
    fstd::stable_partition(list.begin(), list.end(), [](int value) { return value % 3 == 0; });
    REQUIRE(list == std::list<int>{3, 6, 1, 2, 4, 5});
    std::list<int> merge_list{1, 4, 7, 2, 3, 8};
    fstd::inplace_merge(merge_list.begin(), std::next(merge_list.begin(), 3), merge_list.end());
    REQUIRE(merge_list == std::list<int>{1, 2, 3, 4, 7, 8});

    std::vector<int> split{1, 2, 3, 4, 5, 6, 7};
    const auto odd = [](int value) { return value % 2 != 0; };
    const auto boundary = fstd::partition(split.begin(), split.end(), odd);
    REQUIRE((boundary - split.begin() == 4 && fstd::is_partitioned(split.begin(), split.end(), odd)));
    std::forward_list<int> forward{2, 1, 4, 3};
    fstd::partition(forward.begin(), forward.end(), odd);
    REQUIRE(fstd::is_partitioned(forward.begin(), forward.end(), odd));
    std::vector<int> odds;
    std::vector<int> evens;
    fstd::partition_copy(split.begin(), split.end(), fstd::back_inserter(odds),
                         fstd::back_inserter(evens), odd);
    REQUIRE((odds.size() == 4 && evens.size() == 3));
}

TEST_CASE("binary search on sorted ranges", "[libcxx]")
{
    constexpr fstd::array<int, 7> values{1, 2, 2, 2, 5, 7, 9};
    STATIC_REQUIRE(fstd::lower_bound(values.begin(), values.end(), 2) == values.begin() + 1);
    STATIC_REQUIRE(fstd::upper_bound(values.begin(), values.end(), 2) == values.begin() + 4);
    STATIC_REQUIRE(fstd::equal_range(values.begin(), values.end(), 2).second == values.begin() + 4);
    STATIC_REQUIRE(fstd::binary_search(values.begin(), values.end(), 7));
    STATIC_REQUIRE(!fstd::binary_search(values.begin(), values.end(), 6));
    STATIC_REQUIRE(fstd::lower_bound(values.begin(), values.end(), 10) == values.end());

    std::forward_list<int> list{1, 3, 5, 7};
    REQUIRE(*fstd::lower_bound(list.begin(), list.end(), 4) == 5);
    REQUIRE(*fstd::upper_bound(list.begin(), list.end(), 5, std::less<>()) == 7);
}

TEST_CASE("merge and the set operations", "[libcxx]")
{
    const std::vector<int> left{1, 2, 2, 4, 6};
    const std::vector<int> right{2, 3, 4, 4, 7};
    auto run = [&](auto operation, auto reference) {
        std::vector<int> ours;
        std::vector<int> expected;
        operation(left.begin(), left.end(), right.begin(), right.end(), fstd::back_inserter(ours));
        reference(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(expected));
        return ours == expected;
    };
    REQUIRE(run([](auto... args) { return fstd::merge(args...); },
                [](auto... args) { return std::merge(args...); }));
    REQUIRE(run([](auto... args) { return fstd::set_union(args...); },
                [](auto... args) { return std::set_union(args...); }));
    REQUIRE(run([](auto... args) { return fstd::set_intersection(args...); },
                [](auto... args) { return std::set_intersection(args...); }));
    REQUIRE(run([](auto... args) { return fstd::set_difference(args...); },
                [](auto... args) { return std::set_difference(args...); }));
    REQUIRE(run([](auto... args) { return fstd::set_symmetric_difference(args...); },
                [](auto... args) { return std::set_symmetric_difference(args...); }));

    const std::vector<int> subset{2, 4, 6};
    REQUIRE(fstd::includes(left.begin(), left.end(), subset.begin(), subset.end()));
    REQUIRE(!fstd::includes(left.begin(), left.end(), right.begin(), right.end()));

    // merge takes equal elements from the first range first.
    const std::vector<Keyed> first{{1, 0}, {2, 0}};
    const std::vector<Keyed> second{{1, 1}, {2, 1}};
    std::vector<Keyed> merged;
    fstd::merge(first.begin(), first.end(), second.begin(), second.end(), std::back_inserter(merged),
                ByKey);
    REQUIRE(merged == std::vector<Keyed>{{1, 0}, {1, 1}, {2, 0}, {2, 1}});
}

TEST_CASE("the heap operations keep the largest element first", "[libcxx]")
{
    auto values = RandomValues(200, 1000, 7);
    fstd::make_heap(values.begin(), values.end());
    REQUIRE(std::is_heap(values.begin(), values.end()));
    REQUIRE(fstd::is_heap(values.begin(), values.end()));
    values.push_back(5000);
    fstd::push_heap(values.begin(), values.end());
    REQUIRE(values.front() == 5000);
    fstd::pop_heap(values.begin(), values.end());
    REQUIRE((values.back() == 5000 && fstd::is_heap(values.begin(), values.end() - 1)));
    values.pop_back();
    fstd::sort_heap(values.begin(), values.end());
    REQUIRE(std::is_sorted(values.begin(), values.end()));

    std::vector<int> broken{9, 5, 8, 6};
    REQUIRE(fstd::is_heap_until(broken.begin(), broken.end()) == broken.begin() + 3);
}

TEST_CASE("min, max, minmax and clamp", "[libcxx]")
{
    STATIC_REQUIRE(fstd::min(3, 2) == 2 && fstd::max(3, 2) == 3);
    STATIC_REQUIRE(fstd::min({4, 1, 3}) == 1 && fstd::max({4, 1, 3}) == 4);
    STATIC_REQUIRE(fstd::minmax({4, 1, 3}).second == 4);
    STATIC_REQUIRE(fstd::clamp(12, 0, 10) == 10 && fstd::clamp(-1, 0, 10) == 0 &&
                   fstd::clamp(5, 0, 10) == 5);

    // Equal arguments: min and max return the first, minmax returns them in order.
    const Keyed first{1, 0};
    const Keyed second{1, 1};
    REQUIRE(&fstd::min(first, second, ByKey) == &first);
    REQUIRE(&fstd::max(first, second, ByKey) == &first);
    const auto ordered = fstd::minmax(first, second, ByKey);
    REQUIRE((&ordered.first == &first && &ordered.second == &second));

    const std::vector<Keyed> values{{2, 0}, {1, 1}, {3, 2}, {1, 3}, {3, 4}};
    REQUIRE(fstd::min_element(values.begin(), values.end(), ByKey)->tag == 1);
    REQUIRE(fstd::max_element(values.begin(), values.end(), ByKey)->tag == 2);
    const auto [smallest, largest] = fstd::minmax_element(values.begin(), values.end(), ByKey);
    REQUIRE((smallest->tag == 1 && largest->tag == 4));
}

TEST_CASE("lexicographical comparison and permutations", "[libcxx]")
{
    constexpr fstd::array<int, 3> abc{1, 2, 3};
    constexpr fstd::array<int, 2> ab{1, 2};
    STATIC_REQUIRE(fstd::lexicographical_compare(ab.begin(), ab.end(), abc.begin(), abc.end()));
    STATIC_REQUIRE(!fstd::lexicographical_compare(abc.begin(), abc.end(), ab.begin(), ab.end()));
    STATIC_REQUIRE(fstd::lexicographical_compare_three_way(abc.begin(), abc.end(), ab.begin(),
                                                           ab.end()) > 0);
    STATIC_REQUIRE(fstd::lexicographical_compare_three_way(ab.begin(), ab.end(), ab.begin(),
                                                           ab.end()) == 0);

    std::vector<int> ours{1, 2, 2, 3};
    std::vector<int> expected = ours;
    bool more = true;
    int permutations = 0;
    while (more)
    {
        more = fstd::next_permutation(ours.begin(), ours.end());
        REQUIRE(more == std::next_permutation(expected.begin(), expected.end()));
        REQUIRE(ours == expected);
        ++permutations;
    }
    REQUIRE(permutations == 12);
    REQUIRE(!fstd::prev_permutation(ours.begin(), ours.end()));
    REQUIRE(ours == std::vector<int>{3, 2, 2, 1});
    REQUIRE(fstd::prev_permutation(ours.begin(), ours.end()));
    REQUIRE(ours == std::vector<int>{3, 2, 1, 2});
}

TEST_CASE("shuffle and sample draw from a random bit generator", "[libcxx]")
{
    std::vector<int> values(100);
    std::iota(values.begin(), values.end(), 0);
    auto shuffled = values;
    std::mt19937 generator(3);
    fstd::shuffle(shuffled.begin(), shuffled.end(), generator);
    REQUIRE(std::is_permutation(values.begin(), values.end(), shuffled.begin()));
    REQUIRE(shuffled != values);

    // A generator of 8 bits, so the index for 100 elements combines two draws.
    struct Narrow
    {
        using result_type = unsigned char;
        static constexpr result_type min() { return 0; }
        static constexpr result_type max() { return 255; }
        std::mt19937 engine{5};
        result_type operator()() { return static_cast<result_type>(engine()); }
    };
    Narrow narrow;
    std::vector<int> counts(3);
    for (int round = 0; round != 3000; ++round)
    {
        ++counts[fstd::__detail::UniformBelow(narrow, 3)];
        REQUIRE(fstd::__detail::UniformBelow(narrow, 1000) < 1000);
    }
    REQUIRE(std::all_of(counts.begin(), counts.end(), [](int count) { return count > 850; }));

    std::vector<int> picked;
    fstd::sample(values.begin(), values.end(), std::back_inserter(picked), 10, generator);
    REQUIRE((picked.size() == 10 && std::is_sorted(picked.begin(), picked.end())));
    std::vector<int> reservoir(5);
    const int source[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto end =
        fstd::sample(InputOnly{source}, InputOnly{source + 8}, reservoir.begin(), 5, generator);
    REQUIRE(end == reservoir.end());
    std::sort(reservoir.begin(), reservoir.end());
    REQUIRE(std::adjacent_find(reservoir.begin(), reservoir.end()) == reservoir.end());
}
