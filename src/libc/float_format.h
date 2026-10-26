#ifndef FEATHEROS_LIBC_FLOAT_FORMAT_H
#define FEATHEROS_LIBC_FLOAT_FORMAT_H

#include "libc/big_integer.h"
#include "libc/float_bits.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// Exact binary-to-decimal conversion for printf's %f, %e, %g and %a. A binary float is an
// integer times a power of two, so its decimal expansion is finite: the integer part comes from
// dividing a big integer by 10^9, and each fraction digit from multiplying the remainder by 10.
// Rounding then sees every digit and matches glibc, ties going to the even digit.
namespace FloatFormatDetail
{

// Digits past this precision print as zeros; 1100 covers every double exactly.
constexpr int MaximumPrecision = 1100;
// A 128-bit long double has up to 4933 integer digits.
constexpr size_t MaximumIntegerDigits = 4940;
constexpr size_t MaximumDigits = MaximumIntegerDigits + MaximumPrecision + 4;

class DigitStream
{
public:
    explicit DigitStream(const DecodedFloat& value);
    size_t IntegerLength() const;
    // The next digit: integer digits first, then fraction digits forever.
    char Next();
    // True if any digit after the ones read so far is not zero.
    bool Remaining() const;

private:
    static void AddInto(BigInteger* target, const BigInteger& addend);
    BigInteger integer;
    BigInteger fraction;
    size_t fraction_bits;
    char integer_digits[MaximumIntegerDigits];
    size_t integer_length;
    size_t position;
};

// Rounds digits[0, length) using the digit after them and whether anything follows it. Returns
// true if a carry ran off the front, so the digits are now 1 followed by zeros.
bool RoundDigits(char* digits, size_t length, char next, bool more);

struct Digits
{
    char text[MaximumDigits];
    size_t length;
    // %e: the decimal exponent of the first digit. %f: how many digits are before the point.
    int exponent;
};

// %f: the integer digits (at least one) and precision fraction digits.
void FixedDigits(const DecodedFloat& value, int precision, Digits* digits);

// %e: count significant digits and the decimal exponent of the first.
void ScientificDigits(const DecodedFloat& value, int count, Digits* digits);

} // namespace FloatFormatDetail

// Text for one floating-point conversion, without padding: the prefix, the body, the zeros a
// precision asks for beyond the digits kept, then the suffix (an exponent).
struct FloatText
{
    // A sign, then "0x" for %a.
    char prefix[3];
    size_t prefix_length;
    char body[FloatFormatDetail::MaximumDigits + 2];
    size_t body_length;
    size_t trailing_zeros;
    char suffix[12];
    size_t suffix_length;
    bool finite;
};

struct FloatOptions
{
    char conversion;
    // -1 when the format gives none.
    int precision;
    bool plus;
    bool space;
    bool alternate;
};

namespace FloatFormatDetail
{

class TextWriter
{
public:
    explicit TextWriter(FloatText* target);
    void Put(char character);
    void Text(const char* characters, size_t count);
    void Zeros(int requested, int kept);
    void Exponent(char marker, int exponent, int minimum_digits);

private:
    FloatText* text;
};

bool IsUpper(char conversion);
int Kept(int precision);

// digits.exponent digits before the point, the rest after it.
void WriteFixed(TextWriter& writer, const Digits& digits, int precision, bool point,
                bool strip_zeros);

// One digit, the point, and the rest of the significant digits.
void WriteScientific(TextWriter& writer, const Digits& digits, int precision, bool point,
                     bool strip_zeros, char marker);

// %g: %e's exponent X for the precision P decides. P > X >= -4 prints as %f with P - 1 - X
// fraction digits, anything else as %e with P - 1; '#' keeps the trailing zeros.
void WriteGeneral(TextWriter& writer, const DecodedFloat& value, const FloatOptions& options,
                  int precision);

// %a: "1." and the fraction in hex, then the binary exponent. Rounding may carry into the
// leading digit, making it 2, as glibc prints it.
void WriteHexadecimal(TextWriter& writer, const DecodedFloat& value, const FloatOptions& options);

} // namespace FloatFormatDetail

void FormatFloat(const DecodedFloat& value, const FloatOptions& options, FloatText* text);

} // namespace FeatherOS::LibC

#endif
