#include <catch2/catch_test_macros.hpp>

#include <bitset>
#include <deque>
#include <functional>
#include <queue>
#include <random>
#include <stack>
#include <string>
#include <vector>

#include "libcxx/include/bitset"
#include "libcxx/include/deque"
#include "libcxx/include/functional"
#include "libcxx/include/memory"
#include "libcxx/include/queue"
#include "libcxx/include/stack"
#include "libcxx/include/string"
#include "libcxx/include/vector"

namespace
{

namespace fstd = featheros_std;

template <size_t Size> std::string Text(const fstd::bitset<Size>& bits)
{
    const fstd::string text = bits.to_string();
    return {text.data(), text.size()};
}

template <size_t Size> void CompareWithHost(std::mt19937_64& random)
{
    const unsigned long long seed = random();
    fstd::bitset<Size> ours(seed);
    std::bitset<Size> host(seed);
    REQUIRE(Text(ours) == host.to_string());
    for (int round = 0; round != 40; ++round)
    {
        const unsigned long long value = random();
        const size_t shift = random() % (Size + 3);
        const size_t position = random() % (Size == 0 ? 1 : Size);
        switch (random() % 8)
        {
        case 0:
            ours <<= shift;
            host <<= shift;
            break;
        case 1:
            ours >>= shift;
            host >>= shift;
            break;
        case 2:
            ours ^= fstd::bitset<Size>(value);
            host ^= std::bitset<Size>(value);
            break;
        case 3:
            ours |= fstd::bitset<Size>(value) << shift;
            host |= std::bitset<Size>(value) << shift;
            break;
        case 4:
            ours &= ~(fstd::bitset<Size>(value) << shift);
            host &= ~(std::bitset<Size>(value) << shift);
            break;
        case 5:
            ours.flip();
            host.flip();
            break;
        case 6:
            if (Size != 0)
            {
                ours.flip(position);
                host.flip(position);
            }
            break;
        default:
            if (Size != 0)
            {
                ours[position] = (value & 1) != 0;
                host[position] = (value & 1) != 0;
            }
            break;
        }
        REQUIRE(Text(ours) == host.to_string());
        REQUIRE(ours.count() == host.count());
        REQUIRE(ours.all() == host.all());
        REQUIRE(ours.any() == host.any());
        REQUIRE(ours.none() == host.none());
    }
    const auto text = host.to_string();
    REQUIRE(fstd::bitset<Size>(fstd::string(text.c_str())) == ours);
    REQUIRE(fstd::bitset<Size>(text.c_str()) == ours);
}

constexpr fstd::bitset<100> ShiftedInConstantEvaluation()
{
    fstd::bitset<100> bits(0b1011);
    bits <<= 70;
    bits.set(3).reset(73).flip(99);
    return bits;
}

struct Unordered
{
    int value;
};

} // namespace

TEST_CASE("bitset matches the host's under random operations", "[libcxx]")
{
    std::mt19937_64 random(26);
    CompareWithHost<0>(random);
    CompareWithHost<1>(random);
    CompareWithHost<7>(random);
    CompareWithHost<63>(random);
    CompareWithHost<64>(random);
    CompareWithHost<65>(random);
    CompareWithHost<128>(random);
    CompareWithHost<200>(random);
}

TEST_CASE("bitset converts, parses and hashes", "[libcxx]")
{
    STATIC_REQUIRE(ShiftedInConstantEvaluation().count() == 4);
    STATIC_REQUIRE(ShiftedInConstantEvaluation().test(71) && ShiftedInConstantEvaluation()[99]);
    STATIC_REQUIRE(fstd::bitset<4>(0xff).to_ulong() == 0xf);
    STATIC_REQUIRE(fstd::bitset<8>("0x0x", 4, '0', 'x').to_ulong() == 0b0101);

    // Of a string longer than the set, the first characters are the bits; the position and count
    // pick a substring.
    REQUIRE(fstd::bitset<4>("110011").to_ulong() == 0b1100);
    REQUIRE(fstd::bitset<8>(fstd::string("xx1011yy"), 2, 4).to_ulong() == 0b1011);
    REQUIRE(fstd::bitset<8>(fstd::string("101"), 3).none());
    REQUIRE(Text(fstd::bitset<6>(5)) == "000101");
    const fstd::string custom = fstd::bitset<5>(0b10010).to_string('.', '#');
    REQUIRE(std::string(custom.data(), custom.size()) == "#..#.");

    const fstd::bitset<130> high = fstd::bitset<130>(1) << 129;
    REQUIRE(fstd::bitset<130>(~0ULL).to_ullong() == ~0ULL);
    REQUIRE((high >> 129).to_ullong() == 1);
    REQUIRE((high | fstd::bitset<130>(3)).count() == 3);
    REQUIRE((high & fstd::bitset<130>(3)).none());
    REQUIRE((high ^ high).none());
    REQUIRE(fstd::bitset<130>().set().all());
    REQUIRE(fstd::bitset<130>().set().count() == 130);
    REQUIRE(~fstd::bitset<130>() == fstd::bitset<130>().set());

    fstd::bitset<10> bits;
    bits[2] = true;
    bits[3] = bits[2];
    REQUIRE((~bits[4] && !~bits[3]));
    bits[3].flip();
    REQUIRE(bits.to_ulong() == 0b100);
    REQUIRE(bits.size() == 10);

    const fstd::hash<fstd::bitset<130>> hasher;
    REQUIRE(hasher(high) == hasher(fstd::bitset<130>(1) << 129));
    REQUIRE(hasher(high) != hasher(fstd::bitset<130>(1)));
}

TEST_CASE("queue and stack adapt their containers", "[libcxx]")
{
    fstd::queue<int> queue;
    std::queue<int> hostQueue;
    fstd::stack<int> stack;
    std::stack<int> hostStack;
    for (int value = 0; value != 100; ++value)
    {
        if (value % 3 == 2)
        {
            queue.pop();
            hostQueue.pop();
            stack.pop();
            hostStack.pop();
        }
        else
        {
            queue.push(value);
            hostQueue.push(value);
            stack.emplace(value);
            hostStack.emplace(value);
        }
        REQUIRE(queue.size() == hostQueue.size());
        REQUIRE((queue.front() == hostQueue.front() && queue.back() == hostQueue.back()));
        REQUIRE(stack.top() == hostStack.top());
    }

    STATIC_REQUIRE(fstd::is_same_v<fstd::queue<int>::container_type, fstd::deque<int>>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::queue(fstd::vector<char>())),
                                   fstd::queue<char, fstd::vector<char>>>);
    STATIC_REQUIRE(fstd::uses_allocator_v<fstd::stack<int>, fstd::allocator<int>>);
    // A queue of elements that cannot be compared still builds; only its comparisons are gone.
    STATIC_REQUIRE(sizeof(fstd::queue<Unordered>) == sizeof(fstd::deque<Unordered>));
    STATIC_REQUIRE(!fstd::three_way_comparable<fstd::queue<Unordered>>);

    const int values[] = {4, 1, 3};
    fstd::queue<int> fromRange(fstd::from_range, values);
    fstd::queue<int> fromIterators(values, values + 3);
    REQUIRE(fromRange == fromIterators);
    fromIterators.push_range(fstd::vector<int>{9});
    REQUIRE(fromRange < fromIterators);
    REQUIRE((fromRange <=> fromIterators) < 0);
    fstd::stack<int, fstd::vector<int>> onVector(fstd::vector<int>{1, 2});
    onVector.push_range(values);
    REQUIRE((onVector.size() == 5 && onVector.top() == 3));
    fstd::stack<int, fstd::vector<int>> other;
    swap(onVector, other);
    REQUIRE((onVector.empty() && other.size() == 5));
}

TEST_CASE("priority_queue keeps the largest on top", "[libcxx]")
{
    std::mt19937 random(6);
    fstd::priority_queue<int> ours;
    std::priority_queue<int> host;
    for (int round = 0; round != 500; ++round)
    {
        if (!host.empty() && random() % 3 == 0)
        {
            ours.pop();
            host.pop();
        }
        else
        {
            const int value = static_cast<int>(random() % 1000);
            ours.push(value);
            host.push(value);
        }
        REQUIRE(ours.size() == host.size());
        if (!host.empty())
        {
            REQUIRE(ours.top() == host.top());
        }
    }

    const int values[] = {5, 1, 8, 3};
    fstd::priority_queue smallest(values, values + 4, fstd::greater());
    REQUIRE(smallest.top() == 1);
    smallest.emplace(0);
    REQUIRE(smallest.top() == 0);
    fstd::priority_queue<int, fstd::vector<int>, fstd::greater<>> ranged(fstd::from_range, values);
    ranged.push_range(fstd::vector<int>{-2, 9});
    fstd::vector<int> order;
    while (!ranged.empty())
    {
        order.push_back(ranged.top());
        ranged.pop();
    }
    REQUIRE(order == fstd::vector<int>{-2, 1, 3, 5, 8, 9});
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::priority_queue(fstd::less<int>(),
                                                                 fstd::vector<short>())),
                                   fstd::priority_queue<short, fstd::vector<short>,
                                                        fstd::less<int>>>);
}
