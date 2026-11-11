#include <catch2/catch_test_macros.hpp>

#include <map>
#include <random>
#include <set>
#include <string>

#include "libcxx/include/map"
#include "libcxx/include/memory"
#include "libcxx/include/set"
#include "libcxx/include/string"
#include "libcxx/include/string_view"
#include "libcxx/include/vector"

namespace
{

namespace fstd = featheros_std;
using fstd::__detail::TreeNodeBase;

struct Tracked
{
    static inline int alive = 0;
    int value;
    Tracked(int initial = 0) : value(initial) { ++alive; }
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

template <class T> struct CountingAllocator
{
    using value_type = T;
    using propagate_on_container_copy_assignment = fstd::true_type;
    using propagate_on_container_move_assignment = fstd::false_type;
    using propagate_on_container_swap = fstd::true_type;
    using is_always_equal = fstd::false_type;

    int id = 0;
    int* live = nullptr;

    CountingAllocator(int identifier, int* counter) : id(identifier), live(counter) {}
    template <class U>
    CountingAllocator(const CountingAllocator<U>& other) : id(other.id), live(other.live)
    {
    }
    T* allocate(fstd::size_t count)
    {
        ++*live;
        return fstd::allocator<T>().allocate(count);
    }
    void deallocate(T* pointer, fstd::size_t count)
    {
        --*live;
        fstd::allocator<T>().deallocate(pointer, count);
    }
    template <class U> friend bool operator==(const CountingAllocator& left, const CountingAllocator<U>& right)
    {
        return left.id == right.id;
    }
};

class InputIterator
{
public:
    using iterator_category = fstd::input_iterator_tag;
    using value_type = int;
    using difference_type = fstd::ptrdiff_t;
    using pointer = const int*;
    using reference = const int&;

    InputIterator() = default;
    explicit InputIterator(const int* position) : m_position(position) {}
    const int& operator*() const { return *m_position; }
    InputIterator& operator++()
    {
        ++m_position;
        return *this;
    }
    void operator++(int) { ++m_position; }
    friend bool operator==(const InputIterator&, const InputIterator&) = default;

private:
    const int* m_position = nullptr;
};

struct InputRange
{
    const int* first;
    const int* last;
    InputIterator begin() const { return InputIterator(first); }
    InputIterator end() const { return InputIterator(last); }
};

// The black height of the subtree, or -1 when a red-black rule or a parent link is broken.
int BlackHeight(const TreeNodeBase* node, const TreeNodeBase* parent)
{
    if (node == nullptr)
    {
        return 1;
    }
    if (node->parent != parent)
    {
        return -1;
    }
    if (!node->black && ((node->left != nullptr && !node->left->black) ||
                         (node->right != nullptr && !node->right->black)))
    {
        return -1;
    }
    const int left = BlackHeight(node->left, node);
    const int right = BlackHeight(node->right, node);
    if (left < 0 || left != right)
    {
        return -1;
    }
    return left + (node->black ? 1 : 0);
}

// The header is end(): its parent is the root, its left and right the smallest and largest.
template <class Container> bool IsRedBlack(const Container& container)
{
    const TreeNodeBase* const header = container.end().base();
    const TreeNodeBase* const root = header->parent;
    if (root == nullptr)
    {
        return container.empty() && header->left == header && header->right == header;
    }
    if (!root->black || header->left != container.begin().base() ||
        header->right != fstd::prev(container.end()).base())
    {
        return false;
    }
    return BlackHeight(root, header) > 0;
}

template <class Ours, class Theirs> bool SameMap(const Ours& ours, const Theirs& theirs)
{
    if (ours.size() != theirs.size())
    {
        return false;
    }
    auto theirs_position = theirs.begin();
    for (const auto& [key, value] : ours)
    {
        if (key != theirs_position->first || value != theirs_position->second)
        {
            return false;
        }
        ++theirs_position;
    }
    return true;
}

} // namespace

TEST_CASE("map against the host's in random operations", "[libcxx][map]")
{
    fstd::map<int, int> ours;
    std::map<int, int> theirs;
    std::mt19937 random(11);
    for (int step = 0; step != 30000; ++step)
    {
        const int key = static_cast<int>(random() % 800);
        switch (random() % 10)
        {
        case 0:
        {
            const auto [position, inserted] = ours.insert({key, step});
            REQUIRE(inserted == theirs.insert({key, step}).second);
            REQUIRE(position->first == key);
            break;
        }
        case 1:
            REQUIRE(ours.emplace(key, step).second == theirs.emplace(key, step).second);
            break;
        case 2:
        {
            const auto hint = ours.lower_bound(key);
            const auto position = ours.emplace_hint(hint, key, step);
            theirs.emplace_hint(theirs.lower_bound(key), key, step);
            REQUIRE(position->first == key);
            break;
        }
        case 3:
            REQUIRE(ours.try_emplace(key, step).second == theirs.try_emplace(key, step).second);
            break;
        case 4:
            REQUIRE(ours.insert_or_assign(key, step).second ==
                    theirs.insert_or_assign(key, step).second);
            break;
        case 5:
            ours[key] += 3;
            theirs[key] += 3;
            break;
        case 6:
            REQUIRE(ours.erase(key) == theirs.erase(key));
            break;
        case 7:
        {
            const auto position = ours.find(key);
            REQUIRE((position == ours.end()) == (theirs.find(key) == theirs.end()));
            if (position != ours.end())
            {
                const auto next = ours.erase(position);
                const auto host_next = theirs.erase(theirs.find(key));
                REQUIRE((next == ours.end()) == (host_next == theirs.end()));
                if (next != ours.end())
                {
                    REQUIRE(next->first == host_next->first);
                }
            }
            break;
        }
        case 8:
        {
            const auto lower = ours.lower_bound(key);
            const auto upper = ours.upper_bound(key);
            REQUIRE((lower == ours.end() ? -1 : lower->first) ==
                    (theirs.lower_bound(key) == theirs.end() ? -1 : theirs.lower_bound(key)->first));
            REQUIRE((upper == ours.end() ? -1 : upper->first) ==
                    (theirs.upper_bound(key) == theirs.end() ? -1 : theirs.upper_bound(key)->first));
            REQUIRE(ours.count(key) == theirs.count(key));
            break;
        }
        default:
        {
            const int last = key + static_cast<int>(random() % 40);
            ours.erase(ours.lower_bound(key), ours.lower_bound(last));
            theirs.erase(theirs.lower_bound(key), theirs.lower_bound(last));
            break;
        }
        }
        if (step % 500 == 0)
        {
            REQUIRE(SameMap(ours, theirs));
            REQUIRE(IsRedBlack(ours));
        }
    }
    REQUIRE(SameMap(ours, theirs));
    REQUIRE(IsRedBlack(ours));

    std::size_t backwards = 0;
    for (auto position = ours.rbegin(); position != ours.rend(); ++position, ++backwards)
    {
        REQUIRE(position->first == fstd::next(theirs.rbegin(), static_cast<long>(backwards))->first);
    }
}

TEST_CASE("multimap keeps equal keys in insertion order, hints included", "[libcxx][map]")
{
    fstd::multimap<int, int> ours;
    std::multimap<int, int> theirs;
    std::mt19937 random(12);
    for (int step = 0; step != 20000; ++step)
    {
        const int key = static_cast<int>(random() % 60);
        switch (random() % 6)
        {
        case 0:
        case 1:
            REQUIRE(ours.insert({key, step})->second == step);
            theirs.insert({key, step});
            break;
        case 2:
        {
            // A hint anywhere: inside, before or after the key's run.
            const auto distance = static_cast<long>(random() % (ours.size() + 1));
            ours.emplace_hint(fstd::next(ours.begin(), distance), key, step);
            theirs.emplace_hint(std::next(theirs.begin(), distance), key, step);
            break;
        }
        case 3:
            REQUIRE(ours.erase(key) == theirs.erase(key));
            break;
        case 4:
        {
            const auto [first, last] = ours.equal_range(key);
            const auto [host_first, host_last] = theirs.equal_range(key);
            REQUIRE(fstd::distance(first, last) == std::distance(host_first, host_last));
            REQUIRE(ours.count(key) == theirs.count(key));
            break;
        }
        default:
            // find may return any of the equal keys, as the host's does, so this erases the first.
            if (const auto position = ours.find(key); position != ours.end())
            {
                REQUIRE(position->first == key);
                const auto first = ours.lower_bound(key);
                REQUIRE(first->second == theirs.lower_bound(key)->second);
                ours.erase(first);
                theirs.erase(theirs.lower_bound(key));
            }
            break;
        }
    }
    REQUIRE(SameMap(ours, theirs));
    REQUIRE(IsRedBlack(ours));
}

TEST_CASE("map stays balanced with sorted, reversed and alternating input", "[libcxx][map]")
{
    fstd::set<int> ascending;
    fstd::set<int> descending;
    fstd::multiset<int> alternating;
    for (int value = 0; value != 50000; ++value)
    {
        ascending.insert(value);
        descending.insert(-value);
        alternating.insert(value % 2 == 0 ? value : -value);
        alternating.insert(7);
    }
    REQUIRE(IsRedBlack(ascending));
    REQUIRE(IsRedBlack(descending));
    REQUIRE(IsRedBlack(alternating));
    REQUIRE(alternating.count(7) == 50000);

    for (int value = 0; value < 50000; value += 3)
    {
        ascending.erase(value);
    }
    REQUIRE(IsRedBlack(ascending));
    REQUIRE(ascending.size() == 50000 - 16667);
    REQUIRE(*ascending.begin() == 1);

    // 50000 sorted inserts with an end() hint each take constant time.
    fstd::set<int> hinted;
    for (int value = 0; value != 50000; ++value)
    {
        hinted.insert(hinted.end(), value);
    }
    REQUIRE(IsRedBlack(hinted));
    REQUIRE(hinted.size() == 50000);
}

TEST_CASE("set and multiset against the host's", "[libcxx][set]")
{
    fstd::set<int> unique;
    fstd::multiset<int> multi;
    std::set<int> host_unique;
    std::multiset<int> host_multi;
    std::mt19937 random(13);
    for (int step = 0; step != 20000; ++step)
    {
        const int key = static_cast<int>(random() % 300);
        if (random() % 3 != 0)
        {
            REQUIRE(unique.insert(key).second == host_unique.insert(key).second);
            multi.emplace_hint(multi.upper_bound(key), key);
            host_multi.insert(key);
        }
        else
        {
            REQUIRE(unique.erase(key) == host_unique.erase(key));
            REQUIRE(multi.erase(key) == host_multi.erase(key));
        }
    }
    REQUIRE(std::equal(unique.begin(), unique.end(), host_unique.begin(), host_unique.end()));
    REQUIRE(std::equal(multi.begin(), multi.end(), host_multi.begin(), host_multi.end()));
    REQUIRE(IsRedBlack(unique));
    REQUIRE(IsRedBlack(multi));

    STATIC_REQUIRE(fstd::is_same_v<fstd::set<int>::iterator, fstd::set<int>::const_iterator>);
    STATIC_REQUIRE(fstd::bidirectional_iterator<fstd::set<int>::iterator>);
    STATIC_REQUIRE(fstd::bidirectional_iterator<fstd::map<int, int>::iterator>);
    STATIC_REQUIRE(fstd::is_convertible_v<fstd::map<int, int>::iterator, fstd::map<int, int>::const_iterator>);
    STATIC_REQUIRE(!fstd::is_convertible_v<fstd::map<int, int>::const_iterator, fstd::map<int, int>::iterator>);
}

TEST_CASE("map and set look up with transparent comparators", "[libcxx][map]")
{
    fstd::map<fstd::string, int, fstd::less<>> counts{{"apple", 1}, {"pear", 2}, {"plum", 3}};
    REQUIRE(counts.find(fstd::string_view("pear"))->second == 2);
    REQUIRE(counts.find("fig") == counts.end());
    REQUIRE(counts.contains("plum"));
    REQUIRE(counts.count("apple") == 1);
    REQUIRE(counts.lower_bound("b")->first == "pear");
    REQUIRE(counts.upper_bound("pear")->first == "plum");
    REQUIRE(fstd::distance(counts.equal_range("pear").first, counts.equal_range("pear").second) == 1);
    REQUIRE(counts.erase("apple") == 1);
    const auto node = counts.extract(fstd::string_view("plum"));
    REQUIRE(node.mapped() == 3);
    REQUIRE(counts.size() == 1);

    fstd::set<fstd::string, fstd::less<>> names{"ada", "bob"};
    REQUIRE(names.contains("ada"));
    REQUIRE(names.erase(fstd::string_view("bob")) == 1);
}

TEST_CASE("map's members that build in place", "[libcxx][map]")
{
    fstd::map<int, fstd::unique_ptr<int>> owners;
    owners.try_emplace(3, fstd::make_unique<int>(30));
    auto spare = fstd::make_unique<int>(99);
    REQUIRE(!owners.try_emplace(3, fstd::move(spare)).second);
    REQUIRE(spare != nullptr);
    REQUIRE(*owners.at(3) == 30);
    owners.insert_or_assign(3, fstd::make_unique<int>(31));
    REQUIRE(*owners[3] == 31);
    REQUIRE(owners[4] == nullptr);

    fstd::map<int, fstd::string> texts;
    texts.emplace(fstd::piecewise_construct, fstd::forward_as_tuple(1), fstd::forward_as_tuple(3, 'x'));
    texts.emplace(fstd::pair<int, const char*>(2, "two"));
    texts.insert(fstd::pair<int, const char*>(3, "three"));
    const auto hinted = texts.try_emplace(texts.end(), 4, "four");
    REQUIRE(hinted->second == "four");
    REQUIRE(texts.insert_or_assign(texts.begin(), 1, "one")->second == "one");
    REQUIRE(texts.size() == 4);
    REQUIRE(texts.value_comp()(*texts.begin(), *fstd::next(texts.begin())));
    REQUIRE(texts.key_comp()(1, 2));
}

TEST_CASE("map and set node handles and merge", "[libcxx][map]")
{
    fstd::map<int, fstd::string> left{{1, "one"}, {2, "two"}, {3, "three"}};
    fstd::map<int, fstd::string> right{{3, "drei"}};

    auto node = left.extract(1);
    REQUIRE(!node.empty());
    REQUIRE(node.key() == 1);
    node.key() = 4;
    node.mapped() = "four";
    const auto result = right.insert(fstd::move(node));
    REQUIRE(result.inserted);
    REQUIRE(result.position->first == 4);
    REQUIRE(result.node.empty());

    auto clash = left.extract(left.find(3));
    const auto refused = right.insert(fstd::move(clash));
    REQUIRE(!refused.inserted);
    REQUIRE(refused.position->second == "drei");
    REQUIRE(refused.node.mapped() == "three");

    REQUIRE(left.extract(42).empty());

    fstd::map<int, fstd::string, fstd::greater<int>> other{{2, "zwei"}, {5, "fuenf"}};
    left.merge(other);
    REQUIRE(left.size() == 2);
    REQUIRE(left.at(2) == "two");
    REQUIRE(left.at(5) == "fuenf");
    REQUIRE(other.size() == 1);
    REQUIRE(other.begin()->second == "zwei");

    fstd::multimap<int, fstd::string> many{{2, "deux"}};
    many.merge(left);
    REQUIRE(left.empty());
    REQUIRE(many.count(2) == 2);
    REQUIRE(fstd::next(many.begin())->second == "two");
    REQUIRE(IsRedBlack(many));

    fstd::set<int> numbers{1, 2, 3};
    fstd::multiset<int> repeated{2, 2};
    numbers.merge(repeated);
    REQUIRE(repeated.size() == 2);
    auto handle = numbers.extract(2);
    handle.value() = 10;
    numbers.insert(numbers.end(), fstd::move(handle));
    REQUIRE(*numbers.rbegin() == 10);
    repeated.insert(numbers.extract(numbers.begin()));
    REQUIRE(repeated.count(1) == 1);

    // A node handle that still holds a value frees it.
    {
        auto orphan = numbers.extract(3);
        REQUIRE(orphan.value() == 3);
    }
    REQUIRE(numbers.size() == 1);
}

TEST_CASE("map destroys every value and follows its allocator", "[libcxx][map]")
{
    using Allocator = CountingAllocator<fstd::pair<const int, Tracked>>;
    int live = 0;
    {
        fstd::map<int, Tracked, fstd::less<int>, Allocator> first(Allocator(1, &live));
        for (int key = 0; key != 100; ++key)
        {
            first.emplace(key, key * 2);
        }
        REQUIRE(live == 100);
        REQUIRE(Tracked::alive == 100);

        auto copy = first;
        REQUIRE(copy.get_allocator().id == 1);
        REQUIRE(IsRedBlack(copy));
        REQUIRE(Tracked::alive == 200);

        fstd::map<int, Tracked, fstd::less<int>, Allocator> second(Allocator(2, &live));
        second.emplace(1, 1);
        second = copy;
        REQUIRE(second.get_allocator().id == 1);
        REQUIRE(second.size() == 100);

        // Move assignment does not propagate this allocator, so unequal ones move element by
        // element.
        fstd::map<int, Tracked, fstd::less<int>, Allocator> third(Allocator(3, &live));
        third = fstd::move(first);
        REQUIRE(third.get_allocator().id == 3);
        REQUIRE(third.size() == 100);
        REQUIRE(third.at(50).value == 100);

        fstd::map<int, Tracked, fstd::less<int>, Allocator> moved(fstd::move(second));
        REQUIRE(moved.size() == 100);
        REQUIRE(second.empty());
        REQUIRE(IsRedBlack(moved));
        REQUIRE(IsRedBlack(second));
        second.emplace(7, 7);
        REQUIRE(second.size() == 1);

        swap(moved, third);
        REQUIRE(moved.get_allocator().id == 3);
        REQUIRE(IsRedBlack(moved));
        REQUIRE(IsRedBlack(third));

        auto node = moved.extract(5);
        REQUIRE(node.get_allocator().id == 3);
    }
    REQUIRE(live == 0);
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("map and set compare, erase_if and take ranges", "[libcxx][map]")
{
    const fstd::map<int, int> small{{1, 1}, {2, 2}};
    const fstd::map<int, int> large{{1, 1}, {3, 0}};
    REQUIRE(small < large);
    REQUIRE(small == fstd::map<int, int>({{2, 2}, {1, 1}}));
    REQUIRE((small <=> small) == 0);

    fstd::set<int> numbers{5, 1, 4, 2, 3};
    REQUIRE(fstd::erase_if(numbers, [](int value) { return value % 2 == 0; }) == 2);
    REQUIRE(numbers == fstd::set<int>{1, 3, 5});
    REQUIRE((fstd::multiset<int>{1, 1} <=> fstd::multiset<int>{1, 2}) < 0);

    const int values[] = {4, 2, 4, 9};
    fstd::set<int> ranged(fstd::from_range, InputRange{values, values + 4});
    REQUIRE(ranged.size() == 3);
    fstd::multiset<int> multi_ranged;
    multi_ranged.insert_range(InputRange{values, values + 4});
    REQUIRE(multi_ranged.count(4) == 2);
    fstd::set<int> from_input(InputIterator(values), InputIterator(values + 4));
    REQUIRE(from_input == ranged);

    const fstd::vector<fstd::pair<int, char>> pairs{{2, 'b'}, {1, 'a'}};
    fstd::map deduced(pairs.begin(), pairs.end());
    STATIC_REQUIRE(fstd::is_same_v<decltype(deduced), fstd::map<int, char>>);
    fstd::map from_list{fstd::pair{1, 2.0}, fstd::pair{3, 4.0}};
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_list), fstd::map<int, double>>);
    fstd::set deduced_set{3, 1, 2};
    STATIC_REQUIRE(fstd::is_same_v<decltype(deduced_set), fstd::set<int>>);
    fstd::multiset with_compare({1, 2}, fstd::greater<int>());
    STATIC_REQUIRE(fstd::is_same_v<decltype(with_compare), fstd::multiset<int, fstd::greater<int>>>);
    REQUIRE(*with_compare.begin() == 2);
    fstd::map ranged_map(fstd::from_range, pairs);
    STATIC_REQUIRE(fstd::is_same_v<decltype(ranged_map), fstd::map<int, char>>);
    REQUIRE(ranged_map.begin()->second == 'a');
}
