#include "scan.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

namespace ScanDetail
{

bool InSet(const unsigned char* set, int character)
{
    return (set[static_cast<unsigned char>(character) / 8] &
            (1U << (static_cast<unsigned char>(character) % 8))) != 0;
}

const char* ParseSet(const char* format, unsigned char (&set)[32])
{
    bool negate = false;
    if (*format == '^')
    {
        negate = true;
        ++format;
    }
    for (unsigned char& bits : set)
    {
        bits = 0;
    }
    auto add = [&set](unsigned char character)
    {
        set[character / 8] =
            static_cast<unsigned char>(set[character / 8] | (1U << (character % 8)));
    };
    // A ']' first in the set is a member, not the end.
    if (*format == ']')
    {
        add(']');
        ++format;
    }
    for (; *format != '\0' && *format != ']'; ++format)
    {
        const auto first = static_cast<unsigned char>(*format);
        if (format[1] == '-' && format[2] != ']' && format[2] != '\0')
        {
            const auto last = static_cast<unsigned char>(format[2]);
            for (unsigned value = first; value <= last; ++value)
            {
                add(static_cast<unsigned char>(value));
            }
            format += 2;
            continue;
        }
        add(first);
    }
    if (*format != ']')
    {
        return nullptr;
    }
    if (negate)
    {
        for (unsigned char& bits : set)
        {
            bits = static_cast<unsigned char>(~bits);
        }
    }
    return format + 1;
}

bool IsHexDigit(int character)
{
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

int LowerCase(int character)
{
    return character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character;
}

void StoreInteger(char length, bool doubled, bool is_signed, uint64_t value, void* target)
{
    switch (length)
    {
    case 'h':
        if (doubled)
        {
            is_signed ? Store(target, static_cast<signed char>(value))
                      : Store(target, static_cast<unsigned char>(value));
        }
        else
        {
            is_signed ? Store(target, static_cast<short>(value))
                      : Store(target, static_cast<unsigned short>(value));
        }
        return;
    case 'l':
        if (doubled)
        {
            is_signed ? Store(target, static_cast<long long>(value))
                      : Store(target, static_cast<unsigned long long>(value));
        }
        else
        {
            is_signed ? Store(target, static_cast<long>(value))
                      : Store(target, static_cast<unsigned long>(value));
        }
        return;
    case 'j':
        is_signed ? Store(target, static_cast<intmax_t>(value))
                  : Store(target, static_cast<uintmax_t>(value));
        return;
    case 'z':
    case 't':
        is_signed ? Store(target, static_cast<ptrdiff_t>(value))
                  : Store(target, static_cast<size_t>(value));
        return;
    default:
        is_signed ? Store(target, static_cast<int>(value))
                  : Store(target, static_cast<unsigned int>(value));
        return;
    }
}

} // namespace ScanDetail
} // namespace FeatherOS::LibC
