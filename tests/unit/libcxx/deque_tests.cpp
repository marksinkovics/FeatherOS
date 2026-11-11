#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <random>
#include <string>

#include "libcxx/include/array"
#include "libcxx/include/deque"
#include "libcxx/include/memory"
#include "libcxx/include/string"
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
    friend bool operator==(const Tracked& left, const Tracked& right) { return left.value == right.value; }
};

// Large enough that a block holds 16 of them.
struct Wide
{
    int value = 0;
    char padding[60] = {};
    Wide(int initial = 0) : value(initial) {}
    friend bool operator==(const Wide& left, const Wide& right) { return left.value == right.value; }
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

template <class Ours, class Theirs> bool Same(const Ours& ours, const Theirs& theirs)
{
    if (ours.size() != theirs.size())
    {
        return false;
    }
    for (fstd::size_t index = 0; index != ours.size(); ++index)
    {
        if (!(ours[index] == theirs[index]))
        {
            return false;
        }
    }
    return fstd::equal(ours.rbegin(), ours.rend(), theirs.rbegin());
}

template <class T> void RandomOperations(unsigned seed)
{
    fstd::deque<T> ours;
    std::deque<T> theirs;
    std::mt19937 random(seed);
    for (int step = 0; step != 12000; ++step)
    {
        const T value(step);
        const std::size_t position = random() % (theirs.size() + 1);
        const auto offset = static_cast<long>(position);
        switch (random() % 13)
        {
        case 0:
        case 1:
            ours.push_back(value);
            theirs.push_back(value);
            break;
        case 2:
        case 3:
            ours.emplace_front(value);
            theirs.emplace_front(value);
            break;
        case 4:
            if (!theirs.empty())
            {
                ours.pop_front();
                theirs.pop_front();
            }
            break;
        case 5:
            if (!theirs.empty())
            {
                ours.pop_back();
                theirs.pop_back();
            }
            break;
        case 6:
            REQUIRE(*ours.insert(ours.begin() + offset, value) == value);
            theirs.insert(theirs.begin() + offset, value);
            break;
        case 7:
        {
            const std::size_t count = random() % 70;
            ours.insert(ours.begin() + offset, count, value);
            theirs.insert(theirs.begin() + offset, count, value);
            break;
        }
        case 8:
            if (position < theirs.size())
            {
                const auto count = static_cast<long>(std::min<std::size_t>(random() % 90, theirs.size() - position));
                const auto next = ours.erase(ours.begin() + offset, ours.begin() + offset + count);
                REQUIRE(next - ours.begin() == offset);
                theirs.erase(theirs.begin() + offset, theirs.begin() + offset + count);
            }
            break;
        case 9:
            if (!theirs.empty())
            {
                // The new element is one of the deque's own.
                const std::size_t source = random() % theirs.size();
                ours.emplace(ours.begin() + offset, ours[source]);
                theirs.emplace(theirs.begin() + offset, theirs[source]);
            }
            break;
        case 10:
        {
            const T values[3] = {T(step), T(step + 1), T(step + 2)};
            ours.insert(ours.begin() + offset, values, values + 3);
            theirs.insert(theirs.begin() + offset, values, values + 3);
            break;
        }
        case 11:
        {
            const std::size_t count = random() % 400;
            ours.resize(count, value);
            theirs.resize(count, value);
            break;
        }
        default:
            if (step % 1500 == 0)
            {
                ours.shrink_to_fit();
            }
            if (step % 4000 == 0)
            {
                ours.clear();
                theirs.clear();
            }
            break;
        }
        if (step % 50 == 0)
        {
            REQUIRE(Same(ours, theirs));
        }
    }
    REQUIRE(Same(ours, theirs));
}

} // namespace

TEST_CASE("deque against the host's in random operations", "[libcxx][deque]")
{
    RandomOperations<int>(31);
    RandomOperations<Wide>(32);
    RandomOperations<char>(33);
    {
        RandomOperations<Tracked>(34);
    }
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("deque's iterators are random access and its blocks never move", "[libcxx][deque]")
{
    using Deque = fstd::deque<int>;
    STATIC_REQUIRE(fstd::random_access_iterator<Deque::iterator>);
    STATIC_REQUIRE(fstd::random_access_iterator<Deque::const_iterator>);
    STATIC_REQUIRE(fstd::is_convertible_v<Deque::iterator, Deque::const_iterator>);
    STATIC_REQUIRE(!fstd::is_convertible_v<Deque::const_iterator, Deque::iterator>);
    STATIC_REQUIRE(fstd::ranges::random_access_range<Deque>);

    Deque numbers;
    for (int value = 0; value != 1000; ++value)
    {
        numbers.push_back(value);
    }
    int* const middle = &numbers[500];
    for (int value = 0; value != 5000; ++value)
    {
        numbers.push_front(-value);
        numbers.push_back(value);
    }
    REQUIRE(middle == &numbers[5500]);
    REQUIRE(*middle == 500);

    // Erasing at either end leaves the other elements' iterators valid.
    const auto position = numbers.begin() + 5500;
    for (int count = 0; count != 3000; ++count)
    {
        numbers.pop_front();
        numbers.pop_back();
    }
    numbers.erase(numbers.begin(), numbers.begin() + 100);
    REQUIRE(*position == 500);
    REQUIRE(position - numbers.begin() == 2400);

    const Deque& view = numbers;
    REQUIRE(view.end() - view.begin() == static_cast<long>(view.size()));
    REQUIRE((view.begin() < view.end()));
    REQUIRE(view.begin()[2400] == 500);
    REQUIRE(*(2400 + view.begin()) == 500);
    REQUIRE(view.at(2400) == 500);
}

TEST_CASE("deque used as a queue keeps a bounded number of blocks", "[libcxx][deque]")
{
    int live = 0;
    {
        fstd::deque<int, CountingAllocator<int>> queue(CountingAllocator<int>(1, &live));
        for (int value = 0; value != 300; ++value)
        {
            queue.push_back(value);
        }
        const int settled = live;
        for (int value = 0; value != 200000; ++value)
        {
            queue.push_back(value);
            REQUIRE(queue.front() == (value < 300 ? value : value - 300));
            queue.pop_front();
            REQUIRE(live <= settled + 2);
        }
        for (int value = 0; value != 100000; ++value)
        {
            queue.push_front(value);
            queue.pop_back();
        }
        REQUIRE(live <= settled + 2);
        queue.clear();
        REQUIRE(live <= 2);
        queue.shrink_to_fit();
        REQUIRE(live == 0);
        queue.push_front(1);
        REQUIRE(queue.back() == 1);
    }
    REQUIRE(live == 0);
}

TEST_CASE("deque reads arguments that are its own elements", "[libcxx][deque]")
{
    fstd::deque<fstd::string> words;
    for (int index = 0; index != 40; ++index)
    {
        words.push_back(fstd::string(20, static_cast<char>('a' + index % 26)));
    }
    words.push_back(words.front());
    words.push_front(words.back());
    words.insert(words.begin() + 10, 30, words[12]);
    words.insert(words.end() - 3, 30, words[5]);
    words.emplace(words.begin() + 2, words[60]);
    words.resize(200, words[1]);
    REQUIRE(words.front() == fstd::string(20, 'a'));
    REQUIRE(words.back() == words[1]);
    REQUIRE(words[2] == words[61]);
    REQUIRE(fstd::count(words.begin(), words.end(), fstd::string(20, 'a')) >= 3);
}

TEST_CASE("deque's assignment and swap follow the allocator's traits", "[libcxx][deque]")
{
    using Allocator = CountingAllocator<Tracked>;
    int live = 0;
    {
        fstd::deque<Tracked, Allocator> first(Allocator(1, &live));
        for (int value = 0; value != 500; ++value)
        {
            first.emplace_back(value);
        }
        fstd::deque<Tracked, Allocator> second(Allocator(2, &live));
        second.emplace_back(9);
        second = first;
        REQUIRE(second.get_allocator().id == 1);
        REQUIRE(second.size() == 500);

        fstd::deque<Tracked, Allocator> third(Allocator(3, &live));
        third = fstd::move(first);
        REQUIRE(third.get_allocator().id == 3);
        REQUIRE(third[250].value == 250);

        fstd::deque<Tracked, Allocator> moved(fstd::move(second));
        REQUIRE(second.empty());
        second.push_back(Tracked(4));
        REQUIRE(second.front().value == 4);

        swap(moved, third);
        REQUIRE(moved.get_allocator().id == 3);
        REQUIRE(third.get_allocator().id == 1);
        REQUIRE(Tracked::alive == 500 + 500 + 500 + 1);

        fstd::deque<Tracked, Allocator> unequal(fstd::move(moved), Allocator(4, &live));
        REQUIRE(unequal.size() == 500);
    }
    REQUIRE(live == 0);
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("deque of move-only elements", "[libcxx][deque]")
{
    fstd::deque<fstd::unique_ptr<int>> owners;
    for (int value = 0; value != 100; ++value)
    {
        owners.push_back(fstd::make_unique<int>(value));
    }
    owners.insert(owners.begin() + 50, fstd::make_unique<int>(-1));
    owners.emplace(owners.begin() + 10, fstd::make_unique<int>(-2));
    owners.erase(owners.begin() + 80);
    REQUIRE(*owners[10] == -2);
    REQUIRE(*owners[51] == -1);
    REQUIRE(owners.size() == 101);
    fstd::deque<fstd::unique_ptr<int>> taken(fstd::move(owners));
    REQUIRE(*taken.back() == 99);
}

TEST_CASE("deque compares, erases by value and takes ranges", "[libcxx][deque]")
{
    fstd::deque<int> numbers{1, 2, 1, 3, 1};
    REQUIRE(fstd::erase(numbers, 1) == 3);
    REQUIRE(numbers == fstd::deque<int>{2, 3});
    REQUIRE(fstd::erase_if(numbers, [](int value) { return value > 2; }) == 1);
    REQUIRE((fstd::deque<int>{1, 2} <=> fstd::deque<int>{1, 3}) < 0);
    REQUIRE(fstd::deque<int>{1} < fstd::deque<int>{1, 0});

    const int values[] = {4, 5, 6};
    fstd::deque<int> ranged(fstd::from_range, InputRange{values, values + 3});
    ranged.prepend_range(InputRange{values, values + 2});
    ranged.append_range(fstd::array<int, 2>{7, 8});
    REQUIRE(ranged == fstd::deque<int>{4, 5, 4, 5, 6, 7, 8});
    ranged.insert_range(ranged.begin() + 3, InputRange{values, values + 3});
    REQUIRE(ranged == fstd::deque<int>{4, 5, 4, 4, 5, 6, 5, 6, 7, 8});
    ranged.insert(ranged.begin() + 1, InputIterator(values), InputIterator(values + 1));
    REQUIRE(ranged[1] == 4);
    ranged.assign(InputIterator(values), InputIterator(values + 3));
    REQUIRE(ranged == fstd::deque<int>{4, 5, 6});
    ranged.assign_range(fstd::vector<int>{9});
    REQUIRE(ranged == fstd::deque<int>{9});
    ranged.assign({1, 2, 3, 4});
    ranged.assign(2, 0);
    REQUIRE(ranged == fstd::deque<int>{0, 0});

    fstd::deque deduced(values, values + 3);
    STATIC_REQUIRE(fstd::is_same_v<decltype(deduced), fstd::deque<int>>);
    fstd::deque from_range(fstd::from_range, fstd::vector<char>{'a'});
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_range), fstd::deque<char>>);

    fstd::deque<int> sized(5);
    REQUIRE(sized.size() == 5);
    REQUIRE(sized[4] == 0);
    fstd::deque<int> filled(3, 7);
    REQUIRE(filled.back() == 7);
}
