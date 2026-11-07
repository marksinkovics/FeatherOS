#ifndef FEATHEROS_USER_CXX_INTEROP_STD_H
#define FEATHEROS_USER_CXX_INTEROP_STD_H

// libc++ types for cxxinterop. GCC builds user code without a C++ standard library, so only
// Swift's Clang compiles these, and they stay inline.

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FeatherOS::User::Interop
{

using IntSpan = std::span<const int>;
using IntVector = std::vector<int>;

inline std::optional<int> Half(int value)
{
    if (value % 2 != 0)
    {
        return std::nullopt;
    }
    return value / 2;
}

inline IntVector Squares(int count)
{
    IntVector squares;
    for (int value = 0; value < count; ++value)
    {
        squares.push_back(value * value);
    }
    return squares;
}

inline int Sum(IntSpan values)
{
    int sum = 0;
    for (int value : values)
    {
        sum += value;
    }
    return sum;
}

inline int CountVowels(std::string_view text)
{
    int count = 0;
    for (char character : text)
    {
        count += std::string_view("aeiou").find(character) != std::string_view::npos ? 1 : 0;
    }
    return count;
}

} // namespace FeatherOS::User::Interop

#endif
