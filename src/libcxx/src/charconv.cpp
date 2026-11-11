// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/charconv"
#else
#include <charconv>
#endif

#include "libc/float_bits.h"
#include "libc/float_format.h"
#include "libc/float_parse.h"

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace
{

using FeatherOS::LibC::Decode;
using FeatherOS::LibC::DecodedFloat;
using FeatherOS::LibC::FloatKind;
using FeatherOS::LibC::FloatLayout;
using FeatherOS::LibC::FloatOptions;
using FeatherOS::LibC::FloatText;
using FeatherOS::LibC::Mantissa;
using FeatherOS::LibC::ParsedFloat;
using FeatherOS::LibC::ParseFloat;
using FeatherOS::LibC::RawBits;

template <class T> constexpr const FloatLayout& LayoutOf()
{
    if constexpr (is_same_v<T, float>)
    {
        return FeatherOS::LibC::FloatFormat;
    }
    else if constexpr (is_same_v<T, double>)
    {
        return FeatherOS::LibC::DoubleFormat;
    }
    else
    {
        return FeatherOS::LibC::LongDoubleFormat;
    }
}

int SignBit(const FloatLayout& layout)
{
    return layout.exponent_bits + layout.precision - (layout.explicit_leading_bit ? 0 : 1);
}

// max_digits10: enough significant digits for any value to read back unchanged.
int MaximumDigits(const FloatLayout& layout)
{
    return 2 + layout.precision * 30103 / 100000;
}

class Output
{
public:
    Output(char* first, char* last) : m_cursor(first), m_last(last) {}

    void Put(char character)
    {
        if (m_cursor == m_last)
        {
            m_full = true;
            return;
        }
        *m_cursor++ = character;
    }

    void Text(const char* text, size_t count)
    {
        for (size_t index = 0; index != count; ++index)
        {
            Put(text[index]);
        }
    }

    void Repeat(char character, size_t count)
    {
        for (size_t index = 0; index != count && !m_full; ++index)
        {
            Put(character);
        }
    }

    void Exponent(char marker, int exponent, int minimum_digits)
    {
        Put(marker);
        Put(exponent < 0 ? '-' : '+');
        unsigned magnitude =
            exponent < 0 ? 0U - static_cast<unsigned>(exponent) : static_cast<unsigned>(exponent);
        char digits[12];
        int count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        for (; count < minimum_digits; ++count)
        {
            digits[count] = '0';
        }
        while (count > 0)
        {
            Put(digits[--count]);
        }
    }

    to_chars_result Result() const
    {
        if (m_full)
        {
            return {m_last, errc::value_too_large};
        }
        return {m_cursor, errc{}};
    }

private:
    char* m_cursor;
    char* m_last;
    bool m_full = false;
};

// Significant decimal digits d1 d2 ... dn of the value d1.d2...dn * 10^exponent.
struct DecimalDigits
{
    static constexpr int Capacity = FeatherOS::LibC::FloatFormatDetail::MaximumIntegerDigits + 2;
    char text[Capacity];
    int length;
    int exponent;
};

// How a candidate compares with the value it should read back as.
enum class Trip
{
    Exact,
    Below,
    Above,
};

Trip ReadBack(const FloatLayout& layout, Mantissa target, const DecimalDigits& digits)
{
    char text[DecimalDigits::Capacity + 16];
    size_t length = 0;
    text[length++] = digits.text[0];
    text[length++] = '.';
    for (int index = 1; index < digits.length; ++index)
    {
        text[length++] = digits.text[index];
    }
    text[length++] = 'e';
    to_chars_result end = _FSTD::to_chars(text + length, text + sizeof(text) - 1, digits.exponent);
    *end.ptr = '\0';
    const Mantissa bits = ParseFloat(layout, text).bits;
    if (bits == target)
    {
        return Trip::Exact;
    }
    return bits < target ? Trip::Below : Trip::Above;
}

// One unit up or down in the last digit, with carry or borrow; false if nothing is left.
bool StepLastDigit(DecimalDigits& digits, bool up)
{
    int index = digits.length - 1;
    if (up)
    {
        while (index >= 0 && digits.text[index] == '9')
        {
            digits.text[index--] = '0';
        }
        if (index < 0)
        {
            digits.text[0] = '1';
            ++digits.exponent;
            return true;
        }
        ++digits.text[index];
        return true;
    }
    while (index >= 0 && digits.text[index] == '0')
    {
        digits.text[index--] = '9';
    }
    if (index < 0)
    {
        return false;
    }
    --digits.text[index];
    if (digits.text[0] == '0')
    {
        if (digits.length == 1)
        {
            return false;
        }
        for (int shift = 1; shift < digits.length; ++shift)
        {
            digits.text[shift - 1] = digits.text[shift];
        }
        --digits.length;
        --digits.exponent;
    }
    return true;
}

// Every decimal of count significant digits that reads back as the value lies in one interval
// around it, so if any does, either the correctly rounded one or its neighbour on the other
// side of the value does. The rounded one is the nearer, so it wins when both do.
bool FindDigits(const FloatLayout& layout, const DecodedFloat& value, Mantissa target, int count,
                DecimalDigits& out)
{
    FeatherOS::LibC::FloatFormatDetail::Digits digits;
    FeatherOS::LibC::FloatFormatDetail::ScientificDigits(value, count, &digits);
    for (int index = 0; index < count; ++index)
    {
        out.text[index] = digits.text[index];
    }
    out.length = count;
    out.exponent = digits.exponent;
    const Trip trip = ReadBack(layout, target, out);
    if (trip == Trip::Exact)
    {
        return true;
    }
    return StepLastDigit(out, trip == Trip::Below) && ReadBack(layout, target, out) == Trip::Exact;
}

// If some decimal of n digits reads back, so does one of n + 1 (a zero appended), so the
// shortest count can be found by bisection; max_digits10 always works.
void ShortestDigits(const FloatLayout& layout, const DecodedFloat& value, Mantissa target,
                    DecimalDigits& best)
{
    int low = 1;
    int high = MaximumDigits(layout);
    FindDigits(layout, value, target, high, best);
    while (low < high)
    {
        const int middle = low + (high - low) / 2;
        DecimalDigits candidate;
        if (FindDigits(layout, value, target, middle, candidate))
        {
            best = candidate;
            high = middle;
        }
        else
        {
            low = middle + 1;
        }
    }
    while (best.length > 1 && best.text[best.length - 1] == '0')
    {
        --best.length;
    }
}

void WriteScientific(Output& output, const DecimalDigits& digits)
{
    output.Put(digits.text[0]);
    if (digits.length > 1)
    {
        output.Put('.');
        output.Text(digits.text + 1, static_cast<size_t>(digits.length - 1));
    }
    output.Exponent('e', digits.exponent, 2);
}

size_t ScientificLength(const DecimalDigits& digits)
{
    const int magnitude = digits.exponent < 0 ? -digits.exponent : digits.exponent;
    const size_t exponent_digits = magnitude >= 1000 ? 4 : magnitude >= 100 ? 3 : 2;
    return static_cast<size_t>(digits.length) + (digits.length > 1 ? 1 : 0) + 2 + exponent_digits;
}

// Digits laid out without an exponent, as the shortest text that has them.
size_t FixedLength(const DecimalDigits& digits)
{
    if (digits.exponent < 0)
    {
        return 2 + static_cast<size_t>(-digits.exponent - 1) + static_cast<size_t>(digits.length);
    }
    if (digits.length > digits.exponent + 1)
    {
        return static_cast<size_t>(digits.length) + 1;
    }
    return static_cast<size_t>(digits.exponent) + 1;
}

void WriteFixed(Output& output, const DecimalDigits& digits)
{
    if (digits.exponent < 0)
    {
        output.Text("0.", 2);
        output.Repeat('0', static_cast<size_t>(-digits.exponent - 1));
        output.Text(digits.text, static_cast<size_t>(digits.length));
        return;
    }
    const int integer_digits = digits.exponent + 1;
    if (digits.length > integer_digits)
    {
        output.Text(digits.text, static_cast<size_t>(integer_digits));
        output.Put('.');
        output.Text(digits.text + integer_digits,
                    static_cast<size_t>(digits.length - integer_digits));
        return;
    }
    output.Text(digits.text, static_cast<size_t>(digits.length));
    output.Repeat('0', static_cast<size_t>(integer_digits - digits.length));
}

// A whole number in fixed notation, where every candidate has as many characters as digits:
// the shortest digits padded with zeros lose to the value rounded to an integer, which is nearer
// and as long, or to that integer's neighbour on the other side. A value of 2^53 or more is an
// integer, so it prints exactly, as in MSVC's and GNU's libraries; 99...9, one digit shorter,
// may read back too but is not tried, to match them.
void FixedWholeDigits(const FloatLayout& layout, const DecodedFloat& value, Mantissa target,
                      DecimalDigits& digits)
{
    FeatherOS::LibC::FloatFormatDetail::Digits rounded;
    FeatherOS::LibC::FloatFormatDetail::FixedDigits(value, 0, &rounded);
    if (rounded.length > static_cast<size_t>(DecimalDigits::Capacity) || rounded.text[0] == '0')
    {
        return;
    }
    DecimalDigits candidate;
    candidate.length = static_cast<int>(rounded.length);
    candidate.exponent = candidate.length - 1;
    for (size_t index = 0; index < rounded.length; ++index)
    {
        candidate.text[index] = rounded.text[index];
    }
    const Trip trip = ReadBack(layout, target, candidate);
    if (trip != Trip::Exact && (!StepLastDigit(candidate, trip == Trip::Below) ||
                                ReadBack(layout, target, candidate) != Trip::Exact))
    {
        return;
    }
    if (FixedLength(candidate) <= FixedLength(digits))
    {
        digits = candidate;
    }
}

bool WriteSpecial(Output& output, const DecodedFloat& value)
{
    if (value.kind != FloatKind::Infinite && value.kind != FloatKind::NotANumber)
    {
        return false;
    }
    if (value.negative)
    {
        output.Put('-');
    }
    output.Text(value.kind == FloatKind::Infinite ? "inf" : "nan", 3);
    return true;
}

template <class T> to_chars_result ShortestToChars(char* first, char* last, T number, int format)
{
    const FloatLayout& layout = LayoutOf<T>();
    const Mantissa bits = RawBits(number);
    DecodedFloat value = Decode(layout, bits);
    Output output(first, last);
    if (WriteSpecial(output, value))
    {
        return output.Result();
    }
    if (value.negative)
    {
        output.Put('-');
        value.negative = false;
    }
    if (static_cast<chars_format>(format) == chars_format::hex)
    {
        FloatOptions options = {'a', -1, false, false, false};
        FloatText text;
        FeatherOS::LibC::FormatFloat(value, options, &text);
        output.Text(text.body, text.body_length);
        output.Text(text.suffix, text.suffix_length);
        return output.Result();
    }
    if (value.kind == FloatKind::Zero)
    {
        output.Put('0');
        if (static_cast<chars_format>(format) == chars_format::scientific)
        {
            output.Text("e+00", 4);
        }
        return output.Result();
    }
    const Mantissa target = bits & ~(static_cast<Mantissa>(1) << SignBit(layout));
    DecimalDigits digits;
    ShortestDigits(layout, value, target, digits);
    bool fixed = false;
    switch (format)
    {
    case static_cast<int>(chars_format::fixed):
        fixed = true;
        break;
    case static_cast<int>(chars_format::scientific):
        break;
    case static_cast<int>(chars_format::general):
        fixed = digits.exponent >= -4 && digits.exponent < 6;
        break;
    default:
    {
        DecimalDigits whole = digits;
        if (whole.length <= whole.exponent + 1)
        {
            FixedWholeDigits(layout, value, target, whole);
        }
        if (FixedLength(whole) <= ScientificLength(digits))
        {
            WriteFixed(output, whole);
            return output.Result();
        }
        break;
    }
    }
    if (fixed)
    {
        if (digits.length <= digits.exponent + 1)
        {
            FixedWholeDigits(layout, value, target, digits);
        }
        WriteFixed(output, digits);
    }
    else
    {
        WriteScientific(output, digits);
    }
    return output.Result();
}

template <class T>
to_chars_result PrecisionToChars(char* first, char* last, T number, chars_format format,
                                 int precision)
{
    char conversion = 'g';
    switch (format)
    {
    case chars_format::fixed:
        conversion = 'f';
        break;
    case chars_format::scientific:
        conversion = 'e';
        break;
    case chars_format::hex:
        conversion = 'a';
        break;
    default:
        break;
    }
    const FloatOptions options = {conversion, precision < 0 ? -1 : precision, false, false, false};
    FloatText text;
    FeatherOS::LibC::FormatFloat(Decode(LayoutOf<T>(), RawBits(number)), options, &text);
    Output output(first, last);
    // %a's prefix is a sign and "0x"; to_chars keeps only the sign.
    const size_t prefix =
        conversion == 'a' && text.finite ? text.prefix_length - 2 : text.prefix_length;
    output.Text(text.prefix, prefix);
    output.Text(text.body, text.body_length);
    output.Repeat('0', text.trailing_zeros);
    output.Text(text.suffix, text.suffix_length);
    return output.Result();
}

char Lower(char character)
{
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

bool StartsWithWord(const char* cursor, const char* last, const char* word)
{
    for (; *word != '\0'; ++word, ++cursor)
    {
        if (cursor == last || Lower(*cursor) != *word)
        {
            return false;
        }
    }
    return true;
}

unsigned DigitIn(char character, bool hex)
{
    const unsigned digit = __detail::CharconvDigit(character);
    return digit < (hex ? 16U : 10U) ? digit : 99U;
}

// Saturates well beyond any exponent a float can need, so the sum below cannot overflow.
constexpr long long ExponentLimit = 100'000'000;

long long SaturatingAdd(long long value, long long step)
{
    const long long sum = value + step;
    if (sum > ExponentLimit)
    {
        return ExponentLimit;
    }
    if (sum < -ExponentLimit)
    {
        return -ExponentLimit;
    }
    return sum;
}

template <class T>
from_chars_result FloatFromChars(const char* first, const char* last, T& value, chars_format format)
{
    const FloatLayout& layout = LayoutOf<T>();
    const char* cursor = first;
    const bool negative = cursor != last && *cursor == '-';
    if (negative)
    {
        ++cursor;
    }
    const char* special = nullptr;
    const char* special_end = nullptr;
    if (StartsWithWord(cursor, last, "inf"))
    {
        special = "inf";
        special_end = cursor + (StartsWithWord(cursor, last, "infinity") ? 8 : 3);
    }
    else if (StartsWithWord(cursor, last, "nan"))
    {
        special = "nan";
        special_end = cursor + 3;
        if (special_end != last && *special_end == '(')
        {
            const char* scan = special_end + 1;
            while (scan != last && (__detail::CharconvDigit(*scan) < 36 || *scan == '_'))
            {
                ++scan;
            }
            if (scan != last && *scan == ')')
            {
                special_end = scan + 1;
            }
        }
    }
    if (special != nullptr)
    {
        char text[5] = {'-', special[0], special[1], special[2], '\0'};
        value =
            FeatherOS::LibC::FromRawBits<T>(ParseFloat(layout, negative ? text : text + 1).bits);
        return {special_end, errc{}};
    }

    const bool hex = format == chars_format::hex;
    const int kept_limit =
        hex ? static_cast<int>(FeatherOS::LibC::FloatParseDetail::MaximumHexDigits)
            : static_cast<int>(FeatherOS::LibC::FloatParseDetail::MaximumSignificantDigits);
    // The significant digits, at most kept_limit and a sticky 1 for any nonzero ones dropped,
    // as 0.ddd times a power of the base.
    char kept[FeatherOS::LibC::FloatParseDetail::MaximumSignificantDigits + 2];
    int kept_count = 0;
    bool sticky = false;
    bool any_digit = false;
    bool point = false;
    long long point_position = 0;
    for (; cursor != last; ++cursor)
    {
        if (*cursor == '.' && !point)
        {
            point = true;
            continue;
        }
        const unsigned digit = DigitIn(*cursor, hex);
        if (digit == 99U)
        {
            break;
        }
        any_digit = true;
        if (kept_count == 0 && digit == 0)
        {
            if (point)
            {
                point_position = SaturatingAdd(point_position, -1);
            }
            continue;
        }
        if (!point)
        {
            point_position = SaturatingAdd(point_position, 1);
        }
        if (kept_count < kept_limit)
        {
            kept[kept_count++] = *cursor;
        }
        else if (digit != 0)
        {
            sticky = true;
        }
    }
    if (!any_digit)
    {
        return {first, errc::invalid_argument};
    }

    long long exponent = 0;
    const bool may_have_exponent =
        hex || (format & chars_format::scientific) == chars_format::scientific;
    const bool needs_exponent = !hex && format == chars_format::scientific;
    bool has_exponent = false;
    if (may_have_exponent && cursor != last && Lower(*cursor) == (hex ? 'p' : 'e'))
    {
        const char* scan = cursor + 1;
        bool exponent_negative = false;
        if (scan != last && (*scan == '+' || *scan == '-'))
        {
            exponent_negative = *scan == '-';
            ++scan;
        }
        if (scan != last && DigitIn(*scan, false) < 10)
        {
            for (; scan != last && DigitIn(*scan, false) < 10; ++scan)
            {
                exponent =
                    SaturatingAdd(exponent * 10 > ExponentLimit ? ExponentLimit : exponent * 10,
                                  DigitIn(*scan, false));
            }
            if (exponent_negative)
            {
                exponent = -exponent;
            }
            has_exponent = true;
            cursor = scan;
        }
    }
    if (needs_exponent && !has_exponent)
    {
        return {first, errc::invalid_argument};
    }

    if (kept_count == 0)
    {
        char text[3] = {'-', '0', '\0'};
        value =
            FeatherOS::LibC::FromRawBits<T>(ParseFloat(layout, negative ? text : text + 1).bits);
        return {cursor, errc{}};
    }

    char text[sizeof(kept) + 32];
    size_t length = 0;
    if (negative)
    {
        text[length++] = '-';
    }
    text[length++] = '0';
    if (hex)
    {
        text[length++] = 'x';
        text[length++] = '0';
    }
    text[length++] = '.';
    for (int index = 0; index < kept_count; ++index)
    {
        text[length++] = kept[index];
    }
    if (sticky)
    {
        text[length++] = '1';
    }
    text[length++] = hex ? 'p' : 'e';
    const long long scaled =
        hex ? SaturatingAdd(4 * point_position, exponent) : SaturatingAdd(point_position, exponent);
    to_chars_result end = _FSTD::to_chars(text + length, text + sizeof(text) - 1, scaled);
    *end.ptr = '\0';
    const ParsedFloat parsed = ParseFloat(layout, text);
    const FloatKind kind = Decode(layout, parsed.bits).kind;
    if (kind == FloatKind::Infinite || kind == FloatKind::Zero)
    {
        return {cursor, errc::result_out_of_range};
    }
    value = FeatherOS::LibC::FromRawBits<T>(parsed.bits);
    return {cursor, errc{}};
}

} // namespace

to_chars_result to_chars(char* first, char* last, float value) noexcept
{
    return ShortestToChars(first, last, value, 0);
}
to_chars_result to_chars(char* first, char* last, double value) noexcept
{
    return ShortestToChars(first, last, value, 0);
}
to_chars_result to_chars(char* first, char* last, float value, chars_format format) noexcept
{
    return ShortestToChars(first, last, value, static_cast<int>(format));
}
to_chars_result to_chars(char* first, char* last, double value, chars_format format) noexcept
{
    return ShortestToChars(first, last, value, static_cast<int>(format));
}
to_chars_result to_chars(char* first, char* last, float value, chars_format format,
                         int precision) noexcept
{
    return PrecisionToChars(first, last, value, format, precision);
}
to_chars_result to_chars(char* first, char* last, double value, chars_format format,
                         int precision) noexcept
{
    return PrecisionToChars(first, last, value, format, precision);
}
from_chars_result from_chars(const char* first, const char* last, float& value,
                             chars_format format) noexcept
{
    return FloatFromChars(first, last, value, format);
}
from_chars_result from_chars(const char* first, const char* last, double& value,
                             chars_format format) noexcept
{
    return FloatFromChars(first, last, value, format);
}

#if !defined(__x86_64__) || defined(__SSE__)
to_chars_result to_chars(char* first, char* last, long double value) noexcept
{
    return ShortestToChars(first, last, value, 0);
}
to_chars_result to_chars(char* first, char* last, long double value, chars_format format) noexcept
{
    return ShortestToChars(first, last, value, static_cast<int>(format));
}
to_chars_result to_chars(char* first, char* last, long double value, chars_format format,
                         int precision) noexcept
{
    return PrecisionToChars(first, last, value, format, precision);
}
from_chars_result from_chars(const char* first, const char* last, long double& value,
                             chars_format format) noexcept
{
    return FloatFromChars(first, last, value, format);
}
#endif

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
