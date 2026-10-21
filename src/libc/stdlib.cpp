#include "libc/heap_sort.h"
#include "libc/integer_parse.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{

using FeatherOS::LibC::ParsedInteger;

template <typename Signed>
Signed ParseSigned(const char* text, char** end, int base, Signed maximum)
{
    if (!FeatherOS::LibC::IsValidBase(base))
    {
        errno = EINVAL;
        if (end != nullptr)
        {
            *end = const_cast<char*>(text);
        }
        return 0;
    }
    const auto limit = static_cast<uint64_t>(maximum);
    const ParsedInteger parsed = FeatherOS::LibC::ParseInteger(text, base, limit, limit + 1);
    if (end != nullptr)
    {
        *end = const_cast<char*>(parsed.end);
    }
    if (parsed.overflow)
    {
        errno = ERANGE;
    }
    if (!parsed.negative)
    {
        return static_cast<Signed>(parsed.magnitude);
    }
    // Negating the magnitude minus one keeps the most negative value from overflowing.
    return parsed.magnitude == 0 ? 0 : -static_cast<Signed>(parsed.magnitude - 1) - 1;
}

template <typename Unsigned>
Unsigned ParseUnsigned(const char* text, char** end, int base, Unsigned maximum)
{
    if (!FeatherOS::LibC::IsValidBase(base))
    {
        errno = EINVAL;
        if (end != nullptr)
        {
            *end = const_cast<char*>(text);
        }
        return 0;
    }
    const ParsedInteger parsed = FeatherOS::LibC::ParseInteger(text, base, maximum, maximum);
    if (end != nullptr)
    {
        *end = const_cast<char*>(parsed.end);
    }
    if (parsed.overflow)
    {
        errno = ERANGE;
        return maximum;
    }
    // As the standard requires, a minus sign negates the value in unsigned arithmetic.
    const auto value = static_cast<Unsigned>(parsed.magnitude);
    return parsed.negative ? static_cast<Unsigned>(0 - value) : value;
}

uint64_t random_state = 1;

} // namespace

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

long long llabs(long long value)
{
    return value < 0 ? -value : value;
}

div_t div(int numerator, int denominator)
{
    return {numerator / denominator, numerator % denominator};
}

ldiv_t ldiv(long numerator, long denominator)
{
    return {numerator / denominator, numerator % denominator};
}

lldiv_t lldiv(long long numerator, long long denominator)
{
    return {numerator / denominator, numerator % denominator};
}

long strtol(const char* text, char** end, int base)
{
    return ParseSigned<long>(text, end, base, LONG_MAX);
}

long long strtoll(const char* text, char** end, int base)
{
    return ParseSigned<long long>(text, end, base, LLONG_MAX);
}

unsigned long strtoul(const char* text, char** end, int base)
{
    return ParseUnsigned<unsigned long>(text, end, base, ULONG_MAX);
}

unsigned long long strtoull(const char* text, char** end, int base)
{
    return ParseUnsigned<unsigned long long>(text, end, base, ULLONG_MAX);
}

int atoi(const char* text)
{
    return static_cast<int>(strtol(text, nullptr, 10));
}

long atol(const char* text)
{
    return strtol(text, nullptr, 10);
}

long long atoll(const char* text)
{
    return strtoll(text, nullptr, 10);
}

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*))
{
    FeatherOS::LibC::HeapSort(base, count, size, compare);
}

void* bsearch(const void* key, const void* base, size_t count, size_t size,
              int (*compare)(const void*, const void*))
{
    const auto* elements = static_cast<const unsigned char*>(base);
    size_t low = 0;
    size_t high = count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const void* element = elements + middle * size;
        const int order = compare(key, element);
        if (order == 0)
        {
            return const_cast<void*>(element);
        }
        if (order < 0)
        {
            high = middle;
        }
        else
        {
            low = middle + 1;
        }
    }
    return nullptr;
}

// A 64-bit linear congruential generator; its high bits are the most random.
int rand()
{
    random_state = random_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<int>(random_state >> 33);
}

void srand(unsigned int seed)
{
    random_state = seed;
}
