#include <catch2/catch_test_macros.hpp>

#include <map>
#include <random>
#include <set>
#include <vector>

#include "libcxx/include/memory"
#include "libcxx/include/string"
#include "libcxx/include/string_view"
#include "libcxx/include/unordered_map"
#include "libcxx/include/unordered_set"
#include "libcxx/include/vector"

namespace
{

namespace fstd = featheros_std;

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
    friend bool operator==(const Tracked& left, const Tracked& right) { return left.value == right.value; }
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

// Every key in one bucket, so the bucket walks see long runs of unequal keys.
struct CollidingHash
{
    fstd::size_t operator()(int) const { return 42; }
};

struct TextHash
{
    using is_transparent = void;
    fstd::size_t operator()(fstd::string_view text) const { return fstd::hash<fstd::string_view>()(text); }
};

template <class K, class V> const K& Key(const fstd::pair<const K, V>& value) { return value.first; }
inline int Key(int value) { return value; }

// Each element sits in the bucket its key names, the buckets hold every element once, and the
// load factor is within its maximum.
template <class Container> bool BucketsAgree(const Container& container)
{
    fstd::size_t total = 0;
    for (fstd::size_t bucket = 0; bucket != container.bucket_count(); ++bucket)
    {
        fstd::size_t counted = 0;
        for (auto position = container.begin(bucket); position != container.end(bucket); ++position)
        {
            if (container.bucket(Key(*position)) != bucket)
            {
                return false;
            }
            ++counted;
        }
        if (counted != container.bucket_size(bucket))
        {
            return false;
        }
        total += counted;
    }
    return total == container.size() && container.load_factor() <= container.max_load_factor();
}

// Equal keys follow each other on the list.
template <class Container> bool GroupsTogether(const Container& container)
{
    std::set<int> finished;
    bool started = false;
    int current = 0;
    for (const auto& value : container)
    {
        const int key = Key(value);
        if (!started || key != current)
        {
            if (started)
            {
                finished.insert(current);
            }
            if (finished.contains(key))
            {
                return false;
            }
            current = key;
            started = true;
        }
    }
    return true;
}

} // namespace

TEST_CASE("unordered_map against the host's map in random operations", "[libcxx][unordered]")
{
    fstd::unordered_map<int, int> ours;
    std::map<int, int> theirs;
    std::mt19937 random(21);
    for (int step = 0; step != 30000; ++step)
    {
        const int key = static_cast<int>(random() % 2000) * 1024;
        switch (random() % 9)
        {
        case 0:
            REQUIRE(ours.insert({key, step}).second == theirs.insert({key, step}).second);
            break;
        case 1:
            REQUIRE(ours.emplace(key, step).second == theirs.emplace(key, step).second);
            break;
        case 2:
            REQUIRE(ours.try_emplace(key, step).second == theirs.try_emplace(key, step).second);
            break;
        case 3:
            REQUIRE(ours.insert_or_assign(key, step).second == theirs.insert_or_assign(key, step).second);
            break;
        case 4:
            ours[key] += 5;
            theirs[key] += 5;
            break;
        case 5:
            REQUIRE(ours.erase(key) == theirs.erase(key));
            break;
        case 6:
            if (const auto position = ours.find(key); position != ours.end())
            {
                REQUIRE(position->second == theirs.at(key));
                ours.erase(position);
                theirs.erase(key);
            }
            else
            {
                REQUIRE(!theirs.contains(key));
            }
            break;
        case 7:
            REQUIRE(ours.count(key) == theirs.count(key));
            REQUIRE(ours.contains(key) == theirs.contains(key));
            break;
        default:
            if (step % 97 == 0)
            {
                ours.rehash(random() % 64);
            }
            break;
        }
        if (step % 1000 == 0)
        {
            REQUIRE(BucketsAgree(ours));
        }
    }
    REQUIRE(ours.size() == theirs.size());
    for (const auto& [key, value] : theirs)
    {
        REQUIRE(ours.at(key) == value);
    }
    REQUIRE(BucketsAgree(ours));
    REQUIRE(static_cast<fstd::size_t>(fstd::distance(ours.begin(), ours.end())) == ours.size());
}

TEST_CASE("unordered_multimap keeps equal keys together through rehashes", "[libcxx][unordered]")
{
    fstd::unordered_multimap<int, int> ours;
    std::multimap<int, int> theirs;
    std::mt19937 random(22);
    for (int step = 0; step != 20000; ++step)
    {
        const int key = static_cast<int>(random() % 300);
        switch (random() % 5)
        {
        case 0:
        case 1:
            ours.emplace(key, step);
            theirs.emplace(key, step);
            break;
        case 2:
        {
            const auto hint = ours.find(key);
            ours.emplace_hint(hint, key, step);
            theirs.emplace(key, step);
            break;
        }
        case 3:
            REQUIRE(ours.erase(key) == theirs.erase(key));
            break;
        default:
        {
            const auto [first, last] = ours.equal_range(key);
            std::multiset<int> values;
            for (auto position = first; position != last; ++position)
            {
                values.insert(position->second);
            }
            std::multiset<int> host_values;
            for (auto [host_first, host_last] = theirs.equal_range(key); host_first != host_last; ++host_first)
            {
                host_values.insert(host_first->second);
            }
            REQUIRE(values == host_values);
            REQUIRE(ours.count(key) == theirs.count(key));
            break;
        }
        }
    }
    REQUIRE(ours.size() == theirs.size());
    REQUIRE(GroupsTogether(ours));
    REQUIRE(BucketsAgree(ours));

    // A rehash keeps the order within each group.
    std::vector<int> before;
    for (const auto& [key, value] : ours)
    {
        before.push_back(key * 100000 + value);
    }
    ours.rehash(ours.bucket_count() * 8);
    std::map<int, std::vector<int>> groups_before;
    std::map<int, std::vector<int>> groups_after;
    for (int packed : before)
    {
        groups_before[packed / 100000].push_back(packed % 100000);
    }
    for (const auto& [key, value] : ours)
    {
        groups_after[key].push_back(value);
    }
    REQUIRE(groups_before == groups_after);
    REQUIRE(GroupsTogether(ours));
    REQUIRE(BucketsAgree(ours));
}

TEST_CASE("unordered containers with every key in one bucket", "[libcxx][unordered]")
{
    fstd::unordered_set<int, CollidingHash> unique;
    fstd::unordered_multiset<int, CollidingHash> multi;
    for (int value = 0; value != 300; ++value)
    {
        unique.insert(value % 100);
        multi.insert(value % 50);
    }
    REQUIRE(unique.size() == 100);
    REQUIRE(multi.size() == 300);
    REQUIRE(multi.count(7) == 6);
    REQUIRE(GroupsTogether(multi));
    REQUIRE(unique.bucket_size(unique.bucket(5)) == 100);
    for (int value = 0; value != 100; value += 2)
    {
        REQUIRE(unique.erase(value) == 1);
    }
    REQUIRE(unique.size() == 50);
    REQUIRE(!unique.contains(4));
    REQUIRE(unique.contains(5));
    REQUIRE(multi.erase(7) == 6);
    REQUIRE(GroupsTogether(multi));
}

TEST_CASE("unordered containers' bucket interface and hash policy", "[libcxx][unordered]")
{
    fstd::unordered_set<int> empty;
    REQUIRE(empty.bucket_count() == 0);
    REQUIRE(empty.find(3) == empty.end());
    REQUIRE(empty.erase(3) == 0);
    REQUIRE(empty.load_factor() == 0.0f);

    fstd::unordered_set<int> numbers(10);
    REQUIRE(numbers.bucket_count() >= 10);
    for (int value = 0; value != 1000; ++value)
    {
        numbers.insert(value);
    }
    REQUIRE(numbers.load_factor() <= 1.0f);
    numbers.max_load_factor(0.25f);
    REQUIRE(numbers.bucket_count() >= 4000);
    REQUIRE(BucketsAgree(numbers));
    numbers.max_load_factor(4.0f);
    numbers.rehash(0);
    REQUIRE(numbers.bucket_count() >= 250);
    REQUIRE(numbers.bucket_count() < 4000);
    REQUIRE(BucketsAgree(numbers));
    numbers.reserve(5000);
    REQUIRE(numbers.bucket_count() * 4 >= 5000);

    // Elements stay where they are while the buckets change around them.
    const int* const element = &*numbers.find(500);
    numbers.rehash(1 << 14);
    REQUIRE(&*numbers.find(500) == element);

    // So do iterators when the container moves.
    const auto position = numbers.find(9);
    fstd::unordered_set<int> moved(fstd::move(numbers));
    REQUIRE(*position == 9);
    REQUIRE(moved.find(9) == position);
    REQUIRE(numbers.empty());
    numbers.insert(1);
    REQUIRE(numbers.contains(1));

    STATIC_REQUIRE(fstd::forward_iterator<fstd::unordered_map<int, int>::iterator>);
    STATIC_REQUIRE(fstd::forward_iterator<fstd::unordered_set<int>::local_iterator>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::unordered_set<int>::iterator, fstd::unordered_set<int>::const_iterator>);
}

TEST_CASE("unordered containers look up with a transparent hash", "[libcxx][unordered]")
{
    fstd::unordered_map<fstd::string, int, TextHash, fstd::equal_to<>> ages{{"ada", 36}, {"alan", 41}};
    REQUIRE(ages.find(fstd::string_view("ada"))->second == 36);
    REQUIRE(ages.contains("alan"));
    REQUIRE(ages.count("grace") == 0);
    REQUIRE(ages.bucket("ada") == ages.bucket(fstd::string("ada")));
    REQUIRE(fstd::distance(ages.equal_range("ada").first, ages.equal_range("ada").second) == 1);
    REQUIRE(ages.erase("ada") == 1);
    REQUIRE(ages.extract(fstd::string_view("alan")).mapped() == 41);
    REQUIRE(ages.empty());
}

TEST_CASE("unordered node handles, merge and equality", "[libcxx][unordered]")
{
    fstd::unordered_map<int, fstd::string> left{{1, "one"}, {2, "two"}};
    fstd::unordered_map<int, fstd::string> right{{2, "zwei"}};
    auto node = left.extract(1);
    node.key() = 3;
    REQUIRE(right.insert(fstd::move(node)).inserted);
    const auto refused = right.insert(left.extract(2));
    REQUIRE(!refused.inserted);
    REQUIRE(refused.node.mapped() == "two");
    REQUIRE(refused.position->second == "zwei");
    REQUIRE(right.at(3) == "one");

    fstd::unordered_multimap<int, fstd::string> many{{3, "drei"}};
    many.merge(right);
    REQUIRE(right.empty());
    REQUIRE(many.count(3) == 2);

    fstd::unordered_set<int> unique{1, 2, 3};
    fstd::unordered_multiset<int> repeated{3, 3, 4};
    unique.merge(repeated);
    REQUIRE(unique.size() == 4);
    REQUIRE(repeated.size() == 2);
    REQUIRE(repeated.count(3) == 2);

    const fstd::unordered_multiset<int> first{1, 2, 2, 3};
    const fstd::unordered_multiset<int> second{2, 3, 1, 2};
    const fstd::unordered_multiset<int> third{1, 2, 3, 3};
    REQUIRE(first == second);
    REQUIRE(first != third);
    REQUIRE(fstd::unordered_map<int, int>{{1, 2}} != fstd::unordered_map<int, int>{{1, 3}});
    REQUIRE(fstd::unordered_set<int>{1, 2} == fstd::unordered_set<int>{2, 1});

    fstd::unordered_map<int, int> numbers{{1, 1}, {2, 2}, {3, 3}, {4, 4}};
    REQUIRE(fstd::erase_if(numbers, [](const auto& entry) { return entry.first % 2 == 0; }) == 2);
    REQUIRE(numbers.size() == 2);
}

TEST_CASE("unordered_map destroys every value and follows its allocator", "[libcxx][unordered]")
{
    using Allocator = CountingAllocator<fstd::pair<const int, Tracked>>;
    using Map = fstd::unordered_map<int, Tracked, fstd::hash<int>, fstd::equal_to<int>, Allocator>;
    int live = 0;
    {
        Map first(0, Allocator(1, &live));
        for (int key = 0; key != 200; ++key)
        {
            first.emplace(key, key);
        }
        REQUIRE(Tracked::alive == 200);

        Map copy = first;
        REQUIRE(copy == first);
        REQUIRE(copy.get_allocator().id == 1);

        Map other(0, Allocator(2, &live));
        other.emplace(5, 5);
        other = copy;
        REQUIRE(other.get_allocator().id == 1);
        REQUIRE(other.size() == 200);

        Map third(0, Allocator(3, &live));
        third = fstd::move(first);
        REQUIRE(third.get_allocator().id == 3);
        REQUIRE(third.at(150).value == 150);
        REQUIRE(BucketsAgree(third));

        swap(third, copy);
        REQUIRE(third.get_allocator().id == 1);
        REQUIRE(copy.at(10).value == 10);

        Map moved(fstd::move(other), Allocator(4, &live));
        REQUIRE(moved.size() == 200);
        REQUIRE(BucketsAgree(moved));
    }
    REQUIRE(live == 0);
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("unordered containers take ranges and deduce their types", "[libcxx][unordered]")
{
    const fstd::vector<fstd::pair<fstd::string, int>> pairs{{"a", 1}, {"b", 2}, {"a", 3}};
    fstd::unordered_map ranged(fstd::from_range, pairs);
    STATIC_REQUIRE(fstd::is_same_v<decltype(ranged), fstd::unordered_map<fstd::string, int>>);
    REQUIRE(ranged.size() == 2);
    REQUIRE(ranged.at("a") == 1);

    fstd::unordered_multimap multi(pairs.begin(), pairs.end());
    STATIC_REQUIRE(fstd::is_same_v<decltype(multi), fstd::unordered_multimap<fstd::string, int>>);
    REQUIRE(multi.count("a") == 2);

    fstd::unordered_set deduced{1, 2, 3};
    STATIC_REQUIRE(fstd::is_same_v<decltype(deduced), fstd::unordered_set<int>>);
    fstd::unordered_multiset sized({1, 1}, 8);
    STATIC_REQUIRE(fstd::is_same_v<decltype(sized), fstd::unordered_multiset<int>>);
    REQUIRE(sized.bucket_count() >= 8);
    fstd::unordered_map from_list{fstd::pair{1, 'x'}};
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_list), fstd::unordered_map<int, char>>);

    fstd::unordered_map<int, fstd::unique_ptr<int>> owners;
    owners.try_emplace(1, fstd::make_unique<int>(7));
    auto spare = fstd::make_unique<int>(8);
    REQUIRE(!owners.try_emplace(1, fstd::move(spare)).second);
    REQUIRE(spare != nullptr);
    owners.insert_or_assign(1, fstd::move(spare));
    REQUIRE(*owners[1] == 8);
}
