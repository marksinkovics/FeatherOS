#ifndef FEATHEROS_LIBC_INTEGER_PARSE_H
#define FEATHEROS_LIBC_INTEGER_PARSE_H

#include <stdint.h>

namespace FeatherOS::LibC
{

struct ParsedInteger
{
    uint64_t magnitude;
    bool negative;
    bool overflow;
    // Just past the last digit, or the start of the text if there were no digits.
    const char* end;
};

constexpr bool IsSpace(char character)
{
    return character == ' ' || (character >= '\t' && character <= '\r');
}

constexpr uint64_t DigitValue(char character)
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<uint64_t>(character - '0');
    }
    if (character >= 'a' && character <= 'z')
    {
        return static_cast<uint64_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'Z')
    {
        return static_cast<uint64_t>(character - 'A' + 10);
    }
    return UINT64_MAX;
}

constexpr bool IsValidBase(int base)
{
    return base == 0 || (base >= 2 && base <= 36);
}

// The strtol family's grammar: spaces, a sign, then digits. Base 0 picks 16 for "0x", 8 for a
// leading 0, and 10 otherwise. The magnitude saturates at the limit for the sign.
constexpr ParsedInteger ParseInteger(const char* text, int base, uint64_t positive_limit,
                                     uint64_t negative_limit)
{
    ParsedInteger result = {0, false, false, text};
    const char* cursor = text;
    while (IsSpace(*cursor))
    {
        ++cursor;
    }
    if (*cursor == '+' || *cursor == '-')
    {
        result.negative = *cursor == '-';
        ++cursor;
    }
    // "0x" counts as a prefix only when a hex digit follows; otherwise only the "0" is parsed.
    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') &&
        DigitValue(cursor[2]) < 16)
    {
        cursor += 2;
        base = 16;
    }
    else if (base == 0)
    {
        base = cursor[0] == '0' ? 8 : 10;
    }

    const auto radix = static_cast<uint64_t>(base);
    const uint64_t limit = result.negative ? negative_limit : positive_limit;
    const char* digits = cursor;
    for (; DigitValue(*cursor) < radix; ++cursor)
    {
        const uint64_t digit = DigitValue(*cursor);
        if (result.overflow || result.magnitude > (limit - digit) / radix)
        {
            result.overflow = true;
            continue;
        }
        result.magnitude = result.magnitude * radix + digit;
    }
    if (cursor != digits)
    {
        result.end = cursor;
    }
    if (result.overflow)
    {
        result.magnitude = limit;
    }
    return result;
}

} // namespace FeatherOS::LibC

#endif
