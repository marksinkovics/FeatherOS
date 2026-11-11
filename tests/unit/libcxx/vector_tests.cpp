#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

#include "libcxx/include/array"
#include "libcxx/include/memory"
#include "libcxx/include/span"
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
    Tracked& operator=(Tracked&& other) noexcept
    {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~Tracked() { --alive; }
};

// A move constructor that may throw, so growing copies instead, as move_if_noexcept says.
struct ThrowingMove
{
    static inline int copies = 0;
    static inline int moves = 0;
    int value;
    ThrowingMove(int initial) : value(initial) {}
    ThrowingMove(const ThrowingMove& other) : value(other.value) { ++copies; }
    ThrowingMove(ThrowingMove&& other) noexcept(false) : value(other.value) { ++moves; }
    ThrowingMove& operator=(const ThrowingMove&) = default;
};

struct Node
{
    int value = 0;
    fstd::vector<Node> children;
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

// Only one pass, and no iterator_category beyond input.
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

template <class Ours, class Theirs> bool Same(const Ours& ours, const Theirs& theirs)
{
    if (ours.size() != theirs.size())
    {
        return false;
    }
    for (fstd::size_t index = 0; index != ours.size(); ++index)
    {
        if (!(ours[index] == static_cast<typename Ours::value_type>(theirs[index])))
        {
            return false;
        }
    }
    return true;
}

constexpr int ConstantSum()
{
    fstd::vector<int> values;
    for (int index = 0; index != 20; ++index)
    {
        values.push_back(index);
    }
    values.insert(values.begin() + 3, {100, 200});
    values.erase(values.begin(), values.begin() + 2);
    values.resize(25, 1);
    fstd::vector<int> copy = values;
    copy.shrink_to_fit();
    int sum = 0;
    for (int value : copy)
    {
        sum += value;
    }
    return sum;
}

constexpr bool ConstantBits()
{
    fstd::vector<bool> bits(70, false);
    bits[69] = true;
    bits.flip();
    bits.push_back(true);
    bits.insert(bits.begin() + 1, 3, false);
    return bits.size() == 74 && !bits[1] && !bits[3] && bits[4] && !bits[72] && bits[73] &&
           fstd::count(bits.begin(), bits.end(), true) == 70;
}

} // namespace

TEST_CASE("vector constructs, grows and gives its elements back", "[libcxx]")
{
    fstd::vector<int> empty;
    REQUIRE(empty.empty());
    REQUIRE(empty.capacity() == 0);
    REQUIRE(empty.data() == nullptr);
    REQUIRE(empty.begin() == empty.end());

    fstd::vector<int> counted(4);
    REQUIRE((counted.size() == 4 && counted[3] == 0));
    fstd::vector<int> filled(3, 7);
    REQUIRE((filled.size() == 3 && filled.front() == 7 && filled.back() == 7));
    fstd::vector<int> listed{1, 2, 3, 4};
    REQUIRE((listed.size() == 4 && listed.at(2) == 3 && *listed.rbegin() == 4));

    const int raw[] = {5, 6, 7};
    fstd::vector<int> ranged(raw, raw + 3);
    REQUIRE(Same(ranged, std::vector<int>{5, 6, 7}));
    fstd::vector<int> single_pass(InputIterator(raw), InputIterator(raw + 3));
    REQUIRE(Same(single_pass, ranged));

    fstd::vector<int> grown;
    const int* previous = nullptr;
    int moves = 0;
    for (int index = 0; index != 1000; ++index)
    {
        grown.push_back(index);
        if (grown.data() != previous)
        {
            previous = grown.data();
            ++moves;
        }
    }
    REQUIRE(grown.size() == 1000);
    REQUIRE(grown.capacity() >= 1000);
    REQUIRE(moves <= 12);
    REQUIRE(grown[999] == 999);

    REQUIRE(fstd::vector<int>(5).max_size() >= 5);
    REQUIRE(fstd::is_same_v<decltype(fstd::vector{1, 2}), fstd::vector<int>>);
    REQUIRE(fstd::is_same_v<decltype(fstd::vector(raw, raw + 3)), fstd::vector<int>>);
}

TEST_CASE("vector's iterators are contiguous and distinct from pointers", "[libcxx]")
{
    using Vector = fstd::vector<int>;
    STATIC_REQUIRE(fstd::contiguous_iterator<Vector::iterator>);
    STATIC_REQUIRE(fstd::contiguous_iterator<Vector::const_iterator>);
    STATIC_REQUIRE(!fstd::is_same_v<Vector::iterator, int*>);
    STATIC_REQUIRE(fstd::is_convertible_v<Vector::iterator, Vector::const_iterator>);
    STATIC_REQUIRE(!fstd::is_convertible_v<Vector::const_iterator, Vector::iterator>);
    STATIC_REQUIRE(fstd::ranges::contiguous_range<Vector>);
    STATIC_REQUIRE(fstd::ranges::sized_range<const Vector>);

    Vector values{1, 2, 3};
    Vector::const_iterator constant = values.begin();
    REQUIRE(constant == values.begin());
    REQUIRE(values.end() - constant == 3);
    REQUIRE(constant < values.end());
    REQUIRE(fstd::to_address(values.begin() + 1) == values.data() + 1);
    REQUIRE(fstd::span<int>(values).size() == 3);
}

TEST_CASE("vector holds a struct of its own type", "[libcxx]")
{
    Node root;
    root.children.push_back(Node{1, {}});
    root.children.push_back(Node{2, {}});
    root.children[0].children.push_back(Node{3, {}});
    Node copy = root;
    REQUIRE((copy.children.size() == 2 && copy.children[0].children[0].value == 3));
}

TEST_CASE("vector is usable in constant evaluation", "[libcxx]")
{
    STATIC_REQUIRE(ConstantSum() == 190 - 0 - 1 + 300 + 5);
    STATIC_REQUIRE(ConstantBits());
}

TEST_CASE("vector inserts and erases like the host's", "[libcxx]")
{
    std::mt19937 generator(7);
    fstd::vector<int> ours;
    std::vector<int> theirs;
    for (int step = 0; step != 4000; ++step)
    {
        const auto size = theirs.size();
        const auto at = size == 0 ? 0 : generator() % (size + 1);
        const int value = static_cast<int>(generator() % 1000);
        switch (generator() % 10)
        {
        case 0:
            ours.push_back(value);
            theirs.push_back(value);
            break;
        case 1:
            ours.insert(ours.begin() + static_cast<long>(at), value);
            theirs.insert(theirs.begin() + static_cast<long>(at), value);
            break;
        case 2:
        {
            const auto count = generator() % 6;
            ours.insert(ours.begin() + static_cast<long>(at), count, value);
            theirs.insert(theirs.begin() + static_cast<long>(at), count, value);
            break;
        }
        case 3:
        {
            const int block[] = {value, value + 1, value + 2, value + 3, value + 4};
            const auto count = static_cast<long>(generator() % 6);
            ours.insert(ours.begin() + static_cast<long>(at), block, block + count);
            theirs.insert(theirs.begin() + static_cast<long>(at), block, block + count);
            break;
        }
        case 4:
        {
            const int block[] = {value, -value, value * 2};
            ours.insert(ours.begin() + static_cast<long>(at), InputIterator(block),
                        InputIterator(block + 3));
            theirs.insert(theirs.begin() + static_cast<long>(at), block, block + 3);
            break;
        }
        case 5:
            if (size != 0)
            {
                const auto index = generator() % size;
                ours.erase(ours.begin() + static_cast<long>(index));
                theirs.erase(theirs.begin() + static_cast<long>(index));
            }
            break;
        case 6:
            if (size != 0)
            {
                const auto first = generator() % size;
                const auto last = first + generator() % (size - first + 1);
                ours.erase(ours.begin() + static_cast<long>(first),
                           ours.begin() + static_cast<long>(last));
                theirs.erase(theirs.begin() + static_cast<long>(first),
                             theirs.begin() + static_cast<long>(last));
            }
            break;
        case 7:
        {
            const auto count = generator() % (size + 8);
            ours.resize(count, value);
            theirs.resize(count, value);
            break;
        }
        case 8:
            ours.emplace(ours.begin() + static_cast<long>(at), value);
            theirs.emplace(theirs.begin() + static_cast<long>(at), value);
            break;
        default:
            if (size != 0)
            {
                ours.pop_back();
                theirs.pop_back();
            }
            break;
        }
        REQUIRE(Same(ours, theirs));
    }
}

TEST_CASE("vector reads arguments that are its own elements before moving them", "[libcxx]")
{
    fstd::vector<Tracked> values;
    values.reserve(3);
    values.push_back(Tracked(1));
    values.push_back(Tracked(2));
    values.push_back(Tracked(3));
    REQUIRE(values.size() == values.capacity());
    values.push_back(values[0]);
    REQUIRE(values[3].value == 1);

    values.insert(values.begin(), values.back());
    REQUIRE((values[0].value == 1 && values[1].value == 1 && values[4].value == 1));

    values.reserve(values.size() + 10);
    values.insert(values.begin(), values[2]);
    REQUIRE((values[0].value == 2 && values[3].value == 2));

    values.insert(values.begin() + 1, 3, values[3]);
    REQUIRE((values[1].value == 2 && values[3].value == 2 && values[6].value == 2));

    values.shrink_to_fit();
    values.resize(values.size() + 4, values[0]);
    REQUIRE(values.back().value == 2);

    values.emplace(values.begin(), values.back());
    REQUIRE(values[0].value == 2);
    values.clear();
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("vector destroys every element it built", "[libcxx]")
{
    {
        fstd::vector<Tracked> values(5, Tracked(4));
        values.insert(values.begin() + 2, {Tracked(1), Tracked(2), Tracked(3)});
        values.erase(values.begin(), values.begin() + 3);
        fstd::vector<Tracked> copy(values);
        copy = fstd::vector<Tracked>(10);
        values.assign(2, Tracked(9));
        values.swap(copy);
        REQUIRE(values.size() == 10);
        REQUIRE(copy.size() == 2);
        copy.assign({Tracked(1), Tracked(2), Tracked(3), Tracked(4), Tracked(5), Tracked(6)});
        REQUIRE(copy[5].value == 6);
    }
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("vector moves only what moves without throwing", "[libcxx]")
{
    fstd::vector<fstd::unique_ptr<int>> owners;
    for (int index = 0; index != 40; ++index)
    {
        owners.push_back(fstd::make_unique<int>(index));
    }
    owners.insert(owners.begin() + 5, fstd::make_unique<int>(-1));
    owners.erase(owners.begin());
    REQUIRE((*owners[4] == -1 && *owners.back() == 39));

    fstd::vector<ThrowingMove> careful;
    careful.reserve(1);
    careful.emplace_back(1);
    ThrowingMove::copies = 0;
    ThrowingMove::moves = 0;
    careful.emplace_back(2);
    REQUIRE(ThrowingMove::copies == 1);
    REQUIRE(ThrowingMove::moves == 0);
}

TEST_CASE("vector's assignment and swap follow the allocator's traits", "[libcxx]")
{
    int live = 0;
    using Allocator = CountingAllocator<int>;
    {
        fstd::vector<int, Allocator> first({1, 2, 3}, Allocator(1, &live));
        fstd::vector<int, Allocator> second({4, 5}, Allocator(2, &live));
        REQUIRE(live == 2);

        second = first;
        REQUIRE((second.get_allocator().id == 1 && Same(second, first)));

        fstd::vector<int, Allocator> third({7, 8, 9, 10}, Allocator(3, &live));
        const int* storage = third.data();
        first = fstd::move(third);
        REQUIRE((first.get_allocator().id == 1 && first.size() == 4 && first.data() != storage));

        fstd::vector<int, Allocator> fourth(Allocator(1, &live));
        fourth = fstd::move(first);
        REQUIRE((fourth.size() == 4 && first.empty()));

        second.swap(fourth);
        REQUIRE((second.size() == 4 && fourth.size() == 3));

        fstd::vector<int, Allocator> moved(fstd::move(second), Allocator(5, &live));
        REQUIRE((moved.get_allocator().id == 5 && moved.size() == 4));
    }
    REQUIRE(live == 0);
}

TEST_CASE("vector compares and erases by value", "[libcxx]")
{
    fstd::vector<int> left{1, 2, 3};
    fstd::vector<int> right{1, 2, 4};
    REQUIRE(left != right);
    REQUIRE(left < right);
    REQUIRE((left <=> fstd::vector<int>{1, 2}) > 0);
    REQUIRE(left == fstd::vector<int>{1, 2, 3});

    fstd::vector<int> values{1, 2, 3, 2, 5, 2};
    REQUIRE(fstd::erase(values, 2) == 3);
    REQUIRE(Same(values, std::vector<int>{1, 3, 5}));
    REQUIRE(fstd::erase_if(values, [](int value) { return value > 2; }) == 2);
    REQUIRE(Same(values, std::vector<int>{1}));
}

TEST_CASE("vector takes ranges", "[libcxx]")
{
    const std::vector<int> source{1, 2, 3, 4};
    fstd::vector<int> values(fstd::from_range, source);
    REQUIRE(Same(values, source));
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::vector(fstd::from_range, source)),
                                   fstd::vector<int>>);

    const int raw[] = {8, 9};
    values.append_range(InputRange{raw, raw + 2});
    REQUIRE(Same(values, std::vector<int>{1, 2, 3, 4, 8, 9}));

    values.insert_range(values.begin() + 1, fstd::array<int, 2>{5, 6});
    REQUIRE(Same(values, std::vector<int>{1, 5, 6, 2, 3, 4, 8, 9}));
    values.insert_range(values.begin() + 2, InputRange{raw, raw + 2});
    REQUIRE(Same(values, std::vector<int>{1, 5, 8, 9, 6, 2, 3, 4, 8, 9}));

    values.assign_range(fstd::span<const int>(raw, 2));
    REQUIRE(Same(values, std::vector<int>{8, 9}));
    values.assign_range(InputRange{source.data(), source.data() + 4});
    REQUIRE(Same(values, source));
}

// The model is a vector of char: the host's vector<bool> (libc++ 23) shifts a word by its width
// in some inserts, which UBSan stops.
TEST_CASE("vector<bool> packs bits and behaves like a vector of flags", "[libcxx]")
{
    using Bits = fstd::vector<bool>;
    STATIC_REQUIRE(fstd::random_access_iterator<Bits::iterator>);
    STATIC_REQUIRE(fstd::random_access_iterator<Bits::const_iterator>);
    STATIC_REQUIRE(fstd::output_iterator<Bits::iterator, bool>);
    STATIC_REQUIRE(fstd::is_same_v<Bits::const_reference, bool>);

    std::mt19937 generator(11);
    Bits ours;
    std::vector<char> theirs;
    for (int step = 0; step != 3000; ++step)
    {
        const auto size = theirs.size();
        const auto at = size == 0 ? 0 : generator() % (size + 1);
        const bool value = (generator() & 1) != 0;
        const char flag = value ? 1 : 0;
        switch (generator() % 7)
        {
        case 0:
        case 1:
            ours.push_back(value);
            theirs.push_back(flag);
            break;
        case 2:
        {
            const auto count = generator() % 130;
            ours.insert(ours.begin() + static_cast<long>(at), count, value);
            theirs.insert(theirs.begin() + static_cast<long>(at), count, flag);
            break;
        }
        case 3:
            if (size != 0)
            {
                const auto first = generator() % size;
                const auto last = first + generator() % (size - first + 1);
                ours.erase(ours.begin() + static_cast<long>(first),
                           ours.begin() + static_cast<long>(last));
                theirs.erase(theirs.begin() + static_cast<long>(first),
                             theirs.begin() + static_cast<long>(last));
            }
            break;
        case 4:
        {
            const auto count = generator() % (size + 100);
            ours.resize(count, value);
            theirs.resize(count, flag);
            break;
        }
        case 5:
            ours.flip();
            for (char& bit : theirs)
            {
                bit = bit != 0 ? 0 : 1;
            }
            break;
        default:
            if (size != 0)
            {
                const auto index = generator() % size;
                ours[index].flip();
                theirs[index] = theirs[index] != 0 ? 0 : 1;
            }
            break;
        }
        REQUIRE(Same(ours, theirs));
    }

    Bits pattern{true, false, true, true};
    Bits::swap(pattern[0], pattern[1]);
    REQUIRE((!pattern[0] && pattern[1]));
    fstd::reverse(pattern.begin(), pattern.end());
    REQUIRE((pattern[0] && pattern[1] && pattern[2] && !pattern[3]));
    fstd::sort(pattern.begin(), pattern.end());
    REQUIRE((!pattern[0] && pattern[3]));
    REQUIRE(*(pattern.end() - 1));
    REQUIRE((pattern.end() - 3)[1]);
    REQUIRE(pattern.cend() - pattern.cbegin() == 4);
    REQUIRE(fstd::erase(pattern, true) == 3);
    REQUIRE(pattern.size() == 1);
}

TEST_CASE("vector<bool> hashes and compares by its bits alone", "[libcxx]")
{
    fstd::vector<bool> left(100, true);
    fstd::vector<bool> right(130, true);
    right.resize(100);
    REQUIRE(left == right);
    REQUIRE(fstd::hash<fstd::vector<bool>>()(left) == fstd::hash<fstd::vector<bool>>()(right));
    right[99] = false;
    REQUIRE(right < left);
    REQUIRE(fstd::hash<fstd::vector<bool>>()(left) != fstd::hash<fstd::vector<bool>>()(right));

    fstd::vector<bool> copy = left;
    copy.shrink_to_fit();
    REQUIRE((copy == left && copy.capacity() >= 100));
    copy.reserve(1000);
    REQUIRE(copy.capacity() >= 1000);
    copy.assign({true, false});
    REQUIRE((copy.size() == 2 && copy.back() == false));
}
