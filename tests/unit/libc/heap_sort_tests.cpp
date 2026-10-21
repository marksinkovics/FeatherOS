#include <catch2/catch_test_macros.hpp>

#include "libc/heap_sort.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

int CompareInts(const void* left, const void* right)
{
    const int a = *static_cast<const int*>(left);
    const int b = *static_cast<const int*>(right);
    return (a > b) - (a < b);
}

struct Record
{
    uint32_t key;
    char name[13];
};

int CompareRecords(const void* left, const void* right)
{
    const uint32_t a = static_cast<const Record*>(left)->key;
    const uint32_t b = static_cast<const Record*>(right)->key;
    return (a > b) - (a < b);
}

void RequireSorts(std::vector<int> values)
{
    std::vector<int> expected = values;
    std::sort(expected.begin(), expected.end());
    FeatherOS::LibC::HeapSort(values.data(), values.size(), sizeof(int), CompareInts);
    REQUIRE(values == expected);
}

} // namespace

TEST_CASE("heap sort orders small and edge-case inputs")
{
    RequireSorts({});
    RequireSorts({1});
    RequireSorts({2, 1});
    RequireSorts({3, 3, 3});
    RequireSorts({5, 3, 9, 1, 3, 0, -2, 8, 7});
    RequireSorts({1, 2, 3, 4, 5, 6});
    RequireSorts({6, 5, 4, 3, 2, 1});
}

TEST_CASE("heap sort orders a large pseudo-random input")
{
    std::vector<int> values;
    uint32_t state = 12345;
    for (int index = 0; index < 1000; ++index)
    {
        state = state * 1103515245U + 12345U;
        values.push_back(static_cast<int>(state >> 16) % 500 - 250);
    }
    RequireSorts(values);
}

TEST_CASE("heap sort moves whole elements whose size is not a word")
{
    Record records[] = {{30, "thirty"}, {10, "ten"}, {20, "twenty"}, {0, "zero"}};
    FeatherOS::LibC::HeapSort(records, 4, sizeof(Record), CompareRecords);
    REQUIRE(records[0].key == 0);
    REQUIRE(std::string(records[1].name) == "ten");
    REQUIRE(std::string(records[2].name) == "twenty");
    REQUIRE(std::string(records[3].name) == "thirty");
}
