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
    explicit DigitStream(const DecodedFloat& value)
    {
        integer.Set(static_cast<uint64_t>(value.mantissa));
        BigInteger high;
        high.Set(static_cast<uint64_t>(value.mantissa >> 64));
        if (!high.IsZero())
        {
            high.ShiftLeft(64);
            integer.Set(static_cast<uint64_t>(value.mantissa));
            AddInto(&integer, high);
        }
        fraction_bits = 0;
        if (value.exponent >= 0)
        {
            integer.ShiftLeft(static_cast<size_t>(value.exponent));
            fraction.Set(0);
        }
        else
        {
            fraction_bits = static_cast<size_t>(-value.exponent);
            fraction = integer;
            fraction.KeepBitsBelow(fraction_bits);
            integer.ShiftRight(fraction_bits);
        }
        // The integer part's digits, most significant first.
        integer_length = 0;
        char reversed[MaximumIntegerDigits];
        while (!integer.IsZero())
        {
            uint32_t chunk = integer.DivideSmall(1000000000U);
            for (int digit = 0; digit < 9; ++digit)
            {
                reversed[integer_length++] = static_cast<char>('0' + chunk % 10);
                chunk /= 10;
            }
        }
        while (integer_length > 0 && reversed[integer_length - 1] == '0')
        {
            --integer_length;
        }
        for (size_t index = 0; index < integer_length; ++index)
        {
            integer_digits[index] = reversed[integer_length - 1 - index];
        }
        position = 0;
    }

    size_t IntegerLength() const
    {
        return integer_length;
    }

    // The next digit: integer digits first, then fraction digits forever.
    char Next()
    {
        if (position < integer_length)
        {
            return integer_digits[position++];
        }
        ++position;
        if (fraction.IsZero())
        {
            return '0';
        }
        fraction.MultiplyAdd(10, 0);
        const uint64_t digit = fraction.Bits64(fraction_bits) & 0xF;
        fraction.KeepBitsBelow(fraction_bits);
        return static_cast<char>('0' + digit);
    }

    // True if any digit after the ones read so far is not zero.
    bool Remaining() const
    {
        for (size_t index = position; index < integer_length; ++index)
        {
            if (integer_digits[index] != '0')
            {
                return true;
            }
        }
        return !fraction.IsZero();
    }

private:
    static void AddInto(BigInteger* target, const BigInteger& addend)
    {
        // Only used to join the two 64-bit halves of a mantissa, whose bits do not overlap.
        for (size_t bit = 0; bit < addend.BitLength(); ++bit)
        {
            if (addend.Bit(bit))
            {
                target->SetBit(bit);
            }
        }
    }

    BigInteger integer;
    BigInteger fraction;
    size_t fraction_bits;
    char integer_digits[MaximumIntegerDigits];
    size_t integer_length;
    size_t position;
};

// Rounds digits[0, length) using the digit after them and whether anything follows it. Returns
// true if a carry ran off the front, so the digits are now 1 followed by zeros.
inline bool RoundDigits(char* digits, size_t length, char next, bool more)
{
    const bool last_odd = length > 0 && ((digits[length - 1] - '0') & 1) != 0;
    if (next < '5' || (next == '5' && !more && !last_odd))
    {
        return false;
    }
    for (size_t index = length; index-- > 0;)
    {
        if (digits[index] != '9')
        {
            ++digits[index];
            return false;
        }
        digits[index] = '0';
    }
    return true;
}

struct Digits
{
    char text[MaximumDigits];
    size_t length;
    // %e: the decimal exponent of the first digit. %f: how many digits are before the point.
    int exponent;
};

// %f: the integer digits (at least one) and precision fraction digits.
inline void FixedDigits(const DecodedFloat& value, int precision, Digits* digits)
{
    DigitStream stream(value);
    const size_t integer_length = stream.IntegerLength();
    size_t length = 0;
    if (integer_length == 0)
    {
        digits->text[length++] = '0';
    }
    const int kept = precision < MaximumPrecision ? precision : MaximumPrecision;
    const size_t wanted = (integer_length == 0 ? 1 : integer_length) + static_cast<size_t>(kept);
    while (length < wanted)
    {
        digits->text[length++] = stream.Next();
    }
    const char next = stream.Next();
    if (RoundDigits(digits->text, length, next, stream.Remaining()))
    {
        for (size_t index = length; index > 0; --index)
        {
            digits->text[index] = digits->text[index - 1];
        }
        digits->text[0] = '1';
        ++length;
    }
    digits->length = length;
    digits->exponent = static_cast<int>(length) - kept;
}

// %e: count significant digits and the decimal exponent of the first.
inline void ScientificDigits(const DecodedFloat& value, int count, Digits* digits)
{
    DigitStream stream(value);
    int exponent = static_cast<int>(stream.IntegerLength()) - 1;
    char digit = stream.Next();
    if (value.kind == FloatKind::Zero)
    {
        exponent = 0;
    }
    else
    {
        while (digit == '0')
        {
            digit = stream.Next();
            --exponent;
        }
    }
    const size_t wanted =
        static_cast<size_t>(count < MaximumPrecision + 1 ? count : MaximumPrecision + 1);
    size_t length = 0;
    digits->text[length++] = digit;
    while (length < wanted)
    {
        digits->text[length++] = stream.Next();
    }
    if (RoundDigits(digits->text, length, stream.Next(), stream.Remaining()))
    {
        digits->text[0] = '1';
        ++exponent;
    }
    digits->length = length;
    digits->exponent = exponent;
}

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
    explicit TextWriter(FloatText* target) : text(target) {}

    void Put(char character)
    {
        text->body[text->body_length++] = character;
    }

    void Text(const char* characters, size_t count)
    {
        for (size_t index = 0; index < count; ++index)
        {
            Put(characters[index]);
        }
    }

    void Zeros(int requested, int kept)
    {
        if (requested > kept)
        {
            text->trailing_zeros = static_cast<size_t>(requested - kept);
        }
    }

    void Exponent(char marker, int exponent, int minimum_digits)
    {
        char* suffix = text->suffix;
        size_t length = 0;
        suffix[length++] = marker;
        suffix[length++] = exponent < 0 ? '-' : '+';
        unsigned rest = static_cast<unsigned>(exponent < 0 ? -exponent : exponent);
        char digits[8];
        int count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + rest % 10);
            rest /= 10;
        } while (rest != 0);
        for (int padding = count; padding < minimum_digits; ++padding)
        {
            suffix[length++] = '0';
        }
        while (count > 0)
        {
            suffix[length++] = digits[--count];
        }
        text->suffix_length = length;
    }

private:
    FloatText* text;
};

inline bool IsUpper(char conversion)
{
    return conversion >= 'A' && conversion <= 'Z';
}

inline int Kept(int precision)
{
    return precision < MaximumPrecision ? precision : MaximumPrecision;
}

// digits.exponent digits before the point, the rest after it.
inline void WriteFixed(TextWriter& writer, const Digits& digits, int precision, bool point,
                       bool strip_zeros)
{
    const auto integer_length = static_cast<size_t>(digits.exponent);
    size_t length = digits.length;
    if (strip_zeros)
    {
        while (length > integer_length && digits.text[length - 1] == '0')
        {
            --length;
        }
    }
    writer.Text(digits.text, integer_length);
    if (length > integer_length || point)
    {
        writer.Put('.');
    }
    writer.Text(digits.text + integer_length, length - integer_length);
    if (!strip_zeros)
    {
        writer.Zeros(precision, Kept(precision));
    }
}

// One digit, the point, and the rest of the significant digits.
inline void WriteScientific(TextWriter& writer, const Digits& digits, int precision, bool point,
                            bool strip_zeros, char marker)
{
    size_t length = digits.length;
    if (strip_zeros)
    {
        while (length > 1 && digits.text[length - 1] == '0')
        {
            --length;
        }
    }
    writer.Put(digits.text[0]);
    if (length > 1 || point)
    {
        writer.Put('.');
    }
    writer.Text(digits.text + 1, length - 1);
    if (!strip_zeros)
    {
        writer.Zeros(precision, Kept(precision));
    }
    writer.Exponent(marker, digits.exponent, 2);
}

// %g: %e's exponent X for the precision P decides. P > X >= -4 prints as %f with P - 1 - X
// fraction digits, anything else as %e with P - 1; '#' keeps the trailing zeros.
inline void WriteGeneral(TextWriter& writer, const DecodedFloat& value, const FloatOptions& options,
                         int precision)
{
    const int significant = precision == 0 ? 1 : precision;
    Digits digits;
    ScientificDigits(value, significant, &digits);
    const int exponent = digits.exponent;
    if (exponent < significant && exponent >= -4)
    {
        const int fraction_digits = significant - 1 - exponent;
        FixedDigits(value, fraction_digits, &digits);
        WriteFixed(writer, digits, fraction_digits, options.alternate, !options.alternate);
        return;
    }
    WriteScientific(writer, digits, significant - 1, options.alternate, !options.alternate,
                    IsUpper(options.conversion) ? 'E' : 'e');
}

// %a: "1." and the fraction in hex, then the binary exponent. Rounding may carry into the
// leading digit, making it 2, as glibc prints it.
inline void WriteHexadecimal(TextWriter& writer, const DecodedFloat& value,
                             const FloatOptions& options)
{
    const char* digit_set = IsUpper(options.conversion) ? "0123456789ABCDEF" : "0123456789abcdef";
    Mantissa mantissa = value.mantissa;
    int exponent = 0;
    int fraction_bits = 0;
    if (mantissa != 0)
    {
        int top = 127;
        while (((mantissa >> top) & 1) == 0)
        {
            --top;
        }
        // Whole hex digits after the point need a multiple of four fraction bits.
        fraction_bits = (top + 3) / 4 * 4;
        mantissa <<= fraction_bits - top;
        exponent = value.exponent + top;
    }
    int digits = fraction_bits / 4;
    if (options.precision < 0)
    {
        while (digits > 0 && ((mantissa >> ((fraction_bits / 4 - digits) * 4)) & 0xF) == 0)
        {
            --digits;
        }
        mantissa >>= (fraction_bits / 4 - digits) * 4;
        fraction_bits = digits * 4;
    }
    else if (options.precision < digits)
    {
        const int dropped_bits = (digits - options.precision) * 4;
        const Mantissa half = static_cast<Mantissa>(1) << (dropped_bits - 1);
        const Mantissa rest = mantissa & ((half << 1) - 1);
        mantissa >>= dropped_bits;
        if (rest > half || (rest == half && (mantissa & 1) != 0))
        {
            ++mantissa;
        }
        fraction_bits -= dropped_bits;
        digits = options.precision;
    }
    writer.Put(digit_set[static_cast<int>(mantissa >> fraction_bits)]);
    const int requested = options.precision > digits ? options.precision : digits;
    if (requested > 0 || options.alternate)
    {
        writer.Put('.');
    }
    for (int index = 1; index <= digits; ++index)
    {
        writer.Put(digit_set[static_cast<int>((mantissa >> (fraction_bits - 4 * index)) & 0xF)]);
    }
    writer.Zeros(requested, digits);
    writer.Exponent(IsUpper(options.conversion) ? 'P' : 'p', exponent, 1);
}

} // namespace FloatFormatDetail

inline void FormatFloat(const DecodedFloat& value, const FloatOptions& options, FloatText* text)
{
    using namespace FloatFormatDetail;
    text->prefix_length = 0;
    text->body_length = 0;
    text->trailing_zeros = 0;
    text->suffix_length = 0;
    text->finite = value.kind == FloatKind::Zero || value.kind == FloatKind::Finite;
    if (value.negative)
    {
        text->prefix[text->prefix_length++] = '-';
    }
    else if (options.plus)
    {
        text->prefix[text->prefix_length++] = '+';
    }
    else if (options.space)
    {
        text->prefix[text->prefix_length++] = ' ';
    }
    TextWriter writer(text);
    const bool upper = IsUpper(options.conversion);
    if (!text->finite)
    {
        writer.Text(value.kind == FloatKind::Infinite ? (upper ? "INF" : "inf")
                                                      : (upper ? "NAN" : "nan"),
                    3);
        return;
    }
    const char lower =
        static_cast<char>(upper ? options.conversion - 'A' + 'a' : options.conversion);
    if (lower == 'a')
    {
        text->prefix[text->prefix_length++] = '0';
        text->prefix[text->prefix_length++] = upper ? 'X' : 'x';
        WriteHexadecimal(writer, value, options);
        return;
    }
    const int precision = options.precision < 0 ? 6 : options.precision;
    Digits digits;
    if (lower == 'f')
    {
        FixedDigits(value, precision, &digits);
        WriteFixed(writer, digits, precision, options.alternate, false);
    }
    else if (lower == 'e')
    {
        ScientificDigits(value, precision + 1, &digits);
        WriteScientific(writer, digits, precision, options.alternate, false, upper ? 'E' : 'e');
    }
    else
    {
        WriteGeneral(writer, value, options, precision);
    }
}

} // namespace FeatherOS::LibC

#endif
