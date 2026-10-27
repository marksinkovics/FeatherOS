#include "float_parse.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

namespace FloatParseDetail
{

char Lower(char character)
{
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

bool StartsWith(const char* text, const char* word)
{
    for (; *word != '\0'; ++text, ++word)
    {
        if (Lower(*text) != *word)
        {
            return false;
        }
    }
    return true;
}

int HexValue(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    const char lower = Lower(character);
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

bool IsDecimalDigit(char character)
{
    return character >= '0' && character <= '9';
}

bool ReadExponent(const char** text, long* exponent)
{
    const char* cursor = *text + 1;
    bool negative = false;
    if (*cursor == '+' || *cursor == '-')
    {
        negative = *cursor == '-';
        ++cursor;
    }
    if (!IsDecimalDigit(*cursor))
    {
        return false;
    }
    long value = 0;
    for (; IsDecimalDigit(*cursor); ++cursor)
    {
        // Far beyond any float's range, so larger values change nothing.
        if (value < 1000000)
        {
            value = value * 10 + (*cursor - '0');
        }
    }
    *exponent = negative ? -value : value;
    *text = cursor;
    return true;
}

ParsedFloat RoundToLayout(const FloatLayout& layout, bool negative, BigInteger& value,
                          long exponent, bool sticky)
{
    ParsedFloat result = {0, nullptr, false};
    if (value.IsZero())
    {
        result.bits = Encode(layout, negative, 0, 0);
        return result;
    }
    const auto bits = static_cast<long>(value.BitLength());
    const long leading = exponent + bits - 1;
    const int all_ones = (1 << layout.exponent_bits) - 1;
    if (leading > layout.maximum_exponent)
    {
        result.bits = Encode(layout, negative, 0, all_ones);
        result.range_error = true;
        return result;
    }
    // Below the normal range, the lowest representable bit stays fixed, so fewer bits fit.
    long precision = layout.precision;
    if (leading < layout.minimum_exponent)
    {
        precision -= layout.minimum_exponent - leading;
    }
    const long drop = bits - precision;
    Mantissa mantissa = 0;
    bool inexact = sticky;
    if (drop <= 0)
    {
        mantissa = static_cast<Mantissa>(value.Bits64(0)) |
                   (static_cast<Mantissa>(value.Bits64(64)) << 64);
        mantissa <<= -drop;
    }
    else
    {
        const auto dropped = static_cast<size_t>(drop);
        const bool round_bit = value.Bit(dropped - 1);
        inexact = inexact || round_bit || value.AnyBitBelow(dropped - 1);
        const bool below = sticky || value.AnyBitBelow(dropped - 1);
        mantissa = static_cast<Mantissa>(value.Bits64(dropped)) |
                   (static_cast<Mantissa>(value.Bits64(dropped + 64)) << 64);
        if (round_bit && (below || (mantissa & 1) != 0))
        {
            ++mantissa;
        }
    }
    // The exponent of the mantissa's lowest bit.
    long lowest = exponent + drop;
    if (mantissa == 0)
    {
        result.bits = Encode(layout, negative, 0, 0);
        result.range_error = true;
        return result;
    }
    int mantissa_bits = 0;
    for (Mantissa rest = mantissa; rest != 0; rest >>= 1)
    {
        ++mantissa_bits;
    }
    // Rounding up may add a bit: drop it again, which only ever drops a zero.
    if (mantissa_bits > layout.precision)
    {
        mantissa >>= 1;
        ++lowest;
        --mantissa_bits;
    }
    const long top = lowest + mantissa_bits - 1;
    if (top > layout.maximum_exponent)
    {
        result.bits = Encode(layout, negative, 0, all_ones);
        result.range_error = true;
        return result;
    }
    if (top < layout.minimum_exponent)
    {
        // Subnormal: the field holds the value in units of the lowest subnormal bit.
        const long shift = lowest - (layout.minimum_exponent - (layout.precision - 1));
        result.bits = Encode(layout, negative, mantissa << shift, 0);
        result.range_error = inexact;
        return result;
    }
    mantissa <<= layout.precision - mantissa_bits;
    result.bits =
        Encode(layout, negative, mantissa, static_cast<int>(top + layout.maximum_exponent));
    return result;
}

} // namespace FloatParseDetail

} // namespace FeatherOS::LibC

namespace FeatherOS::LibC
{

ParsedFloat ParseFloat(const FloatLayout& layout, const char* text)
{
    using namespace FloatParseDetail;
    ParsedFloat result = {0, text, false};
    const char* cursor = text;
    while (*cursor == ' ' || (*cursor >= '\t' && *cursor <= '\r'))
    {
        ++cursor;
    }
    bool negative = false;
    if (*cursor == '+' || *cursor == '-')
    {
        negative = *cursor == '-';
        ++cursor;
    }
    const int all_ones = (1 << layout.exponent_bits) - 1;
    if (StartsWith(cursor, "inf"))
    {
        result.bits = Encode(layout, negative, 0, all_ones);
        result.end = cursor + (StartsWith(cursor, "infinity") ? 8 : 3);
        return result;
    }
    if (StartsWith(cursor, "nan"))
    {
        // A quiet NaN: the top fraction bit set.
        const int fraction_bits =
            layout.explicit_leading_bit ? layout.precision : layout.precision - 1;
        Mantissa quiet = static_cast<Mantissa>(1) << (fraction_bits - 1);
        if (layout.explicit_leading_bit)
        {
            quiet |= static_cast<Mantissa>(1) << (fraction_bits - 2);
        }
        result.bits =
            quiet | (static_cast<Mantissa>(all_ones) << fraction_bits) |
            (static_cast<Mantissa>(negative ? 1 : 0) << (fraction_bits + layout.exponent_bits));
        result.end = cursor + 3;
        // nan(n-char-sequence): the characters are accepted and ignored.
        if (*result.end == '(')
        {
            const char* close = result.end + 1;
            while ((*close >= '0' && *close <= '9') ||
                   (Lower(*close) >= 'a' && Lower(*close) <= 'z') || *close == '_')
            {
                ++close;
            }
            if (*close == ')')
            {
                result.end = close + 1;
            }
        }
        return result;
    }

    BigInteger value;
    value.Set(0);
    bool sticky = false;
    bool any_digit = false;
    if (cursor[0] == '0' && Lower(cursor[1]) == 'x' &&
        (HexValue(cursor[2]) >= 0 || (cursor[2] == '.' && HexValue(cursor[3]) >= 0)))
    {
        cursor += 2;
        long exponent = 0;
        size_t kept = 0;
        bool seen_point = false;
        for (;; ++cursor)
        {
            if (*cursor == '.' && !seen_point)
            {
                seen_point = true;
                continue;
            }
            const int digit = HexValue(*cursor);
            if (digit < 0)
            {
                break;
            }
            any_digit = true;
            if (kept < MaximumHexDigits && (kept > 0 || digit != 0))
            {
                value.MultiplyAdd(16, static_cast<uint32_t>(digit));
                ++kept;
                exponent -= seen_point ? 4 : 0;
            }
            else if (kept >= MaximumHexDigits)
            {
                sticky = sticky || digit != 0;
                exponent += seen_point ? 0 : 4;
            }
            else if (seen_point)
            {
                exponent -= 4;
            }
        }
        long binary_exponent = 0;
        if (Lower(*cursor) == 'p' && ReadExponent(&cursor, &binary_exponent))
        {
            exponent += binary_exponent;
        }
        ParsedFloat rounded = RoundToLayout(layout, negative, value, exponent, sticky);
        rounded.end = cursor;
        return rounded;
    }

    long exponent = 0;
    size_t kept = 0;
    bool seen_point = false;
    for (;; ++cursor)
    {
        if (*cursor == '.' && !seen_point)
        {
            seen_point = true;
            continue;
        }
        if (!IsDecimalDigit(*cursor))
        {
            break;
        }
        any_digit = true;
        const int digit = *cursor - '0';
        if (kept == 0 && digit == 0)
        {
            exponent -= seen_point ? 1 : 0;
            continue;
        }
        if (kept < MaximumSignificantDigits)
        {
            value.MultiplyAdd(10, static_cast<uint32_t>(digit));
            ++kept;
            exponent -= seen_point ? 1 : 0;
        }
        else
        {
            sticky = sticky || digit != 0;
            exponent += seen_point ? 0 : 1;
        }
    }
    if (!any_digit)
    {
        return result;
    }
    long decimal_exponent = 0;
    if (Lower(*cursor) == 'e' && ReadExponent(&cursor, &decimal_exponent))
    {
        exponent += decimal_exponent;
    }

    ParsedFloat rounded = {Encode(layout, negative, 0, 0), cursor, false};
    if (value.IsZero())
    {
        return rounded;
    }
    // Decimal exponents beyond these limits overflow or underflow for every layout used here,
    // which keeps the big integers within their capacity.
    const long leading_digit = exponent + static_cast<long>(kept) - 1;
    if (leading_digit > 4940)
    {
        rounded.bits = Encode(layout, negative, 0, all_ones);
        rounded.range_error = true;
        return rounded;
    }
    if (leading_digit < -4980)
    {
        rounded.range_error = true;
        return rounded;
    }
    if (exponent >= 0)
    {
        value.MultiplyByPowerOfTen(static_cast<uint32_t>(exponent));
        rounded = RoundToLayout(layout, negative, value, 0, sticky);
        rounded.end = cursor;
        return rounded;
    }
    // value / 10^-exponent, as a quotient of precision + 3 bits and a remainder.
    BigInteger denominator;
    denominator.Set(1);
    denominator.MultiplyByPowerOfTen(static_cast<uint32_t>(-exponent));
    const long quotient_bits = layout.precision + 3;
    long shift = quotient_bits + static_cast<long>(denominator.BitLength()) -
                 static_cast<long>(value.BitLength()) - 1;
    if (shift >= 0)
    {
        value.ShiftLeft(static_cast<size_t>(shift));
    }
    else
    {
        denominator.ShiftLeft(static_cast<size_t>(-shift));
    }
    // Long division, one quotient bit at a time from the top; value < denominator * 2^bits.
    BigInteger remainder = value;
    remainder.ShiftRight(static_cast<size_t>(quotient_bits));
    BigInteger quotient;
    quotient.Set(0);
    for (long bit = quotient_bits - 1; bit >= 0; --bit)
    {
        remainder.ShiftLeft(1);
        if (value.Bit(static_cast<size_t>(bit)))
        {
            remainder.SetBit(0);
        }
        if (remainder.Compare(denominator) >= 0)
        {
            remainder.Subtract(denominator);
            quotient.SetBit(static_cast<size_t>(bit));
        }
    }
    rounded = RoundToLayout(layout, negative, quotient, -shift, sticky || !remainder.IsZero());
    rounded.end = cursor;
    return rounded;
}

} // namespace FeatherOS::LibC
