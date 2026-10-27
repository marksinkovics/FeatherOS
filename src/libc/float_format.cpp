#include "float_format.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

namespace FloatFormatDetail
{

DigitStream::DigitStream(const DecodedFloat& value)
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

size_t DigitStream::IntegerLength() const
{
    return integer_length;
}

char DigitStream::Next()
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

bool DigitStream::Remaining() const
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

void DigitStream::AddInto(BigInteger* target, const BigInteger& addend)
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

bool RoundDigits(char* digits, size_t length, char next, bool more)
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

void FixedDigits(const DecodedFloat& value, int precision, Digits* digits)
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

void ScientificDigits(const DecodedFloat& value, int count, Digits* digits)
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

TextWriter::TextWriter(FloatText* target) : text(target) {}

void TextWriter::Put(char character)
{
    text->body[text->body_length++] = character;
}

void TextWriter::Text(const char* characters, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        Put(characters[index]);
    }
}

void TextWriter::Zeros(int requested, int kept)
{
    if (requested > kept)
    {
        text->trailing_zeros = static_cast<size_t>(requested - kept);
    }
}

void TextWriter::Exponent(char marker, int exponent, int minimum_digits)
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

bool IsUpper(char conversion)
{
    return conversion >= 'A' && conversion <= 'Z';
}

int Kept(int precision)
{
    return precision < MaximumPrecision ? precision : MaximumPrecision;
}

void WriteFixed(TextWriter& writer, const Digits& digits, int precision, bool point,
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

void WriteScientific(TextWriter& writer, const Digits& digits, int precision, bool point,
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

void WriteGeneral(TextWriter& writer, const DecodedFloat& value, const FloatOptions& options,
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

void WriteHexadecimal(TextWriter& writer, const DecodedFloat& value, const FloatOptions& options)
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

} // namespace FeatherOS::LibC

namespace FeatherOS::LibC
{

void FormatFloat(const DecodedFloat& value, const FloatOptions& options, FloatText* text)
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
