#ifndef FEATHEROS_LIBC_FLOAT_PARSE_H
#define FEATHEROS_LIBC_FLOAT_PARSE_H

#include "libc/big_integer.h"
#include "libc/float_bits.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

struct ParsedFloat
{
    Mantissa bits;
    // Just past the number, or the start of the text if there was none.
    const char* end;
    // Overflow to infinity, or an inexact result below the normal range.
    bool range_error;
};

// strtod's grammar for any layout, correctly rounded (ties to even). Decimal input becomes a big
// integer times a power of ten, and the binary result comes from exact big-integer arithmetic.
// Digits beyond the first 800 only matter for ties, so they are kept as a sticky bit.
ParsedFloat ParseFloat(const FloatLayout& layout, const char* text);

namespace FloatParseDetail
{

constexpr size_t MaximumSignificantDigits = 800;
constexpr size_t MaximumHexDigits = 40;

char Lower(char character);
bool StartsWith(const char* text, const char* word);
int HexValue(char character);
bool IsDecimalDigit(char character);

// Reads an optional sign and digits after an exponent marker; leaves text alone if there are
// no digits, since then the marker is not part of the number.
bool ReadExponent(const char** text, long* exponent);

// Rounds value * 2^exponent (with sticky standing for nonzero bits below it) to the layout.
ParsedFloat RoundToLayout(const FloatLayout& layout, bool negative, BigInteger& value,
                          long exponent, bool sticky);

} // namespace FloatParseDetail

} // namespace FeatherOS::LibC

#endif
