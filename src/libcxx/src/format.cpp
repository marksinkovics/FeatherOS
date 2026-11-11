// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/charconv"
#include "libcxx/include/format"
#else
#include <charconv>
#include <format>
#endif

#include "libc/float_bits.h"
#include "libc/float_format.h"

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace __detail
{

void FormatBuffer::Write(const char* text, size_t length)
{
    while (length != 0)
    {
        if (m_size == m_capacity)
        {
            Flush();
        }
        const size_t room = m_capacity - m_size;
        const size_t chunk = length < room ? length : room;
        for (size_t index = 0; index != chunk; ++index)
        {
            m_data[m_size + index] = text[index];
        }
        m_size += chunk;
        text += chunk;
        length -= chunk;
    }
}

void FormatBuffer::Fill(size_t count, const char* fill, size_t fill_size)
{
    for (size_t copy = 0; copy != count; ++copy)
    {
        for (size_t index = 0; index != fill_size; ++index)
        {
            Put(fill[index]);
        }
    }
}

void FormatBuffer::Flush()
{
    if (m_size != 0)
    {
        m_drain(*this, m_data, m_size);
        m_size = 0;
    }
}

namespace
{

using FeatherOS::LibC::Decode;
using FeatherOS::LibC::DecodedFloat;
using FeatherOS::LibC::FloatKind;
using FeatherOS::LibC::FloatLayout;
using FeatherOS::LibC::FloatOptions;
using FeatherOS::LibC::FloatText;
using FeatherOS::LibC::RawBits;

int DynamicValue(const format_context& context, int id)
{
    long long value = -1;
    bool integer = false;
    visit_format_arg(
        [&](auto argument)
        {
            using T = decltype(argument);
            if constexpr (is_same_v<T, int> || is_same_v<T, long long>)
            {
                integer = true;
                value = argument;
            }
            else if constexpr (is_same_v<T, unsigned> || is_same_v<T, unsigned long long>)
            {
                integer = true;
                value = argument > static_cast<unsigned long long>(numeric_limits<int>::max())
                            ? static_cast<long long>(numeric_limits<int>::max()) + 1
                            : static_cast<long long>(argument);
            }
        },
        context.arg(static_cast<size_t>(id)));
    if (!integer)
    {
        FormatError("a width or precision argument is missing or not an integer");
    }
    if (value < 0)
    {
        FormatError("a width or precision argument is negative");
    }
    if (value > numeric_limits<int>::max())
    {
        FormatError("a width or precision argument is too large");
    }
    return static_cast<int>(value);
}

FormatSpec Resolve(const format_context& context, const FormatSpec& spec)
{
    FormatSpec resolved = spec;
    if (spec.width_argument >= 0)
    {
        resolved.width = DynamicValue(context, spec.width_argument);
    }
    if (spec.precision_argument >= 0)
    {
        resolved.precision = DynamicValue(context, spec.precision_argument);
    }
    return resolved;
}

FormatBuffer& BufferOf(format_context& context)
{
    return context.out().Buffer();
}

// The estimated width of [format.string.std] as C++20 listed it: these ranges take two
// columns and everything else one. ExtendingRanges, a subset of Grapheme_Extend (the combining
// marks of Latin, Cyrillic, Hebrew, Arabic, Devanagari, Thai and kana, joiners and variation
// selectors), take none after another code point, a rough stand-in for grapheme clusters.
struct CodePointRange
{
    char32_t first;
    char32_t last;
};

constexpr CodePointRange WideRanges[] = {
    {0x1100, 0x115F},   {0x2329, 0x232A},   {0x2E80, 0x303E},   {0x3040, 0xA4CF},
    {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

constexpr CodePointRange ExtendingRanges[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD},   {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},   {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8},   {0x06EA, 0x06ED}, {0x0900, 0x0902},
    {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948},   {0x094D, 0x094D}, {0x0951, 0x0957},
    {0x0962, 0x0963}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},   {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF}, {0x200C, 0x200D}, {0x20D0, 0x20FF},   {0x302A, 0x302F}, {0x3099, 0x309A},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF},
};

// What a debug string escapes as \u{...}: General_Category Cc, Cf, Cs, Co, Zs, Zl and Zp, and of
// the unassigned (Cn) only the noncharacters, U+FFF0..U+FFF8 and the unassigned planes; other
// unassigned code points are written as they are.
constexpr CodePointRange EscapedRanges[] = {
    {0x0000, 0x001F},   {0x007F, 0x00A0},   {0x00AD, 0x00AD},    {0x0600, 0x0605},
    {0x061C, 0x061C},   {0x06DD, 0x06DD},   {0x070F, 0x070F},    {0x0890, 0x0891},
    {0x08E2, 0x08E2},   {0x1680, 0x1680},   {0x180E, 0x180E},    {0x2000, 0x200F},
    {0x2028, 0x202F},   {0x205F, 0x206F},   {0x3000, 0x3000},    {0xD800, 0xF8FF},
    {0xFDD0, 0xFDEF},   {0xFEFF, 0xFEFF},   {0xFFF0, 0xFFFB},    {0x110BD, 0x110BD},
    {0x110CD, 0x110CD}, {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3},  {0x1D173, 0x1D17A},
    {0x40000, 0xDFFFF}, {0xE0000, 0xE00FF}, {0xE01F0, 0x10FFFF},
};

template <size_t Count> bool InRanges(const CodePointRange (&ranges)[Count], char32_t code_point)
{
    for (const CodePointRange& range : ranges)
    {
        if (code_point >= range.first && code_point <= range.last)
        {
            return true;
        }
    }
    return false;
}

struct Measured
{
    size_t bytes;
    size_t width;
};

// The longest start of text that fits in maximum columns.
Measured Measure(string_view text, size_t maximum)
{
    const char* const first = text.data();
    const char* const last = first + text.size();
    size_t width = 0;
    const char* cursor = first;
    while (cursor != last)
    {
        const Utf8Decoded decoded = DecodeUtf8(cursor, last);
        size_t columns = 1;
        if (decoded.length != 0 && cursor != first && InRanges(ExtendingRanges, decoded.code_point))
        {
            columns = 0;
        }
        else if (decoded.length != 0 && InRanges(WideRanges, decoded.code_point))
        {
            columns = 2;
        }
        if (width + columns > maximum)
        {
            break;
        }
        width += columns;
        cursor += decoded.length == 0 ? 1 : decoded.length;
    }
    return {static_cast<size_t>(cursor - first), width};
}

void WritePadded(FormatBuffer& out, const FormatSpec& spec, string_view text, size_t width,
                 FormatAlign default_align)
{
    const auto wanted = static_cast<size_t>(spec.width);
    const size_t padding = wanted > width ? wanted - width : 0;
    const FormatAlign align = spec.align == FormatAlign::Default ? default_align : spec.align;
    const size_t before = align == FormatAlign::Left     ? 0
                          : align == FormatAlign::Center ? padding / 2
                                                         : padding;
    out.Fill(before, spec.fill, spec.fill_size);
    out.Write(text);
    out.Fill(padding - before, spec.fill, spec.fill_size);
}

// A number's text: the sign and base prefix that zero padding goes after, the digits, zeros a
// precision asks for past the digits kept, and an exponent.
struct NumberText
{
    string_view prefix;
    string_view body;
    size_t zeros = 0;
    string_view suffix;
};

void WriteNumber(FormatBuffer& out, const FormatSpec& spec, const NumberText& number,
                 bool zero_padding)
{
    const size_t length =
        number.prefix.size() + number.body.size() + number.zeros + number.suffix.size();
    const auto wanted = static_cast<size_t>(spec.width);
    const size_t padding = wanted > length ? wanted - length : 0;
    // An alignment turns the 0 option off.
    const bool zeros = spec.zero && spec.align == FormatAlign::Default && zero_padding;
    const FormatAlign align = spec.align == FormatAlign::Default ? FormatAlign::Right : spec.align;
    const size_t before = zeros                          ? 0
                          : align == FormatAlign::Left   ? 0
                          : align == FormatAlign::Center ? padding / 2
                                                         : padding;
    out.Fill(before, spec.fill, spec.fill_size);
    out.Write(number.prefix);
    if (zeros)
    {
        out.Fill(padding, "0", 1);
    }
    out.Write(number.body);
    out.Fill(number.zeros, "0", 1);
    out.Write(number.suffix);
    if (!zeros)
    {
        out.Fill(padding - before, spec.fill, spec.fill_size);
    }
}

size_t PutSign(char* target, const FormatSpec& spec, bool negative)
{
    if (negative)
    {
        *target = '-';
    }
    else if (spec.sign == FormatSign::Plus)
    {
        *target = '+';
    }
    else if (spec.sign == FormatSign::Space)
    {
        *target = ' ';
    }
    else
    {
        return 0;
    }
    return 1;
}

bool IsEscaped(char32_t code_point)
{
    return (code_point & 0xFFFE) == 0xFFFE || InRanges(EscapedRanges, code_point);
}

void WriteHexEscape(FormatBuffer& out, char kind, char32_t value)
{
    char digits[8];
    const char* const end =
        to_chars(digits, digits + sizeof(digits), static_cast<unsigned>(value), 16).ptr;
    out.Put('\\');
    out.Put(kind);
    out.Put('{');
    out.Write(digits, static_cast<size_t>(end - digits));
    out.Put('}');
}

// [format.string.escaped]: quote is '"' for a string and '\'' for a character, and only that one
// of the two is escaped.
void WriteEscaped(FormatBuffer& out, string_view text, char quote)
{
    out.Put(quote);
    const char* cursor = text.data();
    const char* const last = cursor + text.size();
    // A Grapheme_Extend character stays as it is only after a character that did.
    bool previous_plain = false;
    while (cursor != last)
    {
        const Utf8Decoded decoded = DecodeUtf8(cursor, last);
        if (decoded.length == 0)
        {
            WriteHexEscape(out, 'x', static_cast<unsigned char>(*cursor));
            ++cursor;
            previous_plain = false;
            continue;
        }
        const char32_t code_point = decoded.code_point;
        const char* shorthand = nullptr;
        switch (code_point)
        {
        case U'\t':
            shorthand = "\\t";
            break;
        case U'\n':
            shorthand = "\\n";
            break;
        case U'\r':
            shorthand = "\\r";
            break;
        case U'\\':
            shorthand = "\\\\";
            break;
        case U'"':
            shorthand = quote == '"' ? "\\\"" : nullptr;
            break;
        case U'\'':
            shorthand = quote == '\'' ? "\\'" : nullptr;
            break;
        default:
            break;
        }
        if (shorthand != nullptr)
        {
            out.Write(shorthand, 2);
            previous_plain = false;
        }
        else if (IsEscaped(code_point) ||
                 (!previous_plain && InRanges(ExtendingRanges, code_point)))
        {
            WriteHexEscape(out, 'u', code_point);
            previous_plain = false;
        }
        else
        {
            out.Write(cursor, decoded.length);
            previous_plain = true;
        }
        cursor += decoded.length;
    }
    out.Put(quote);
}

void WriteCharacter(FormatBuffer& out, const FormatSpec& spec, char value)
{
    WritePadded(out, spec, string_view(&value, 1), 1, FormatAlign::Left);
}

void WriteInteger(FormatBuffer& out, const FormatSpec& spec, unsigned long long magnitude,
                  bool negative)
{
    if (spec.type == 'c')
    {
        // char is signed on x86-64 and unsigned on AArch64.
        constexpr auto highest = static_cast<unsigned long long>(
            static_cast<unsigned char>(numeric_limits<char>::max()));
        constexpr unsigned long long lowest = numeric_limits<char>::is_signed ? highest + 1 : 0;
        if (negative ? magnitude > lowest : magnitude > highest)
        {
            FormatError("an integer formatted as a character is out of its range");
        }
        const auto bits = static_cast<unsigned char>(negative ? 0ULL - magnitude : magnitude);
        WriteCharacter(out, spec, static_cast<char>(bits));
        return;
    }
    int base = 10;
    const char* base_prefix = "";
    switch (spec.type)
    {
    case 'b':
    case 'B':
        base = 2;
        base_prefix = spec.type == 'b' ? "0b" : "0B";
        break;
    case 'o':
        base = 8;
        base_prefix = magnitude != 0 ? "0" : "";
        break;
    case 'x':
    case 'X':
        base = 16;
        base_prefix = spec.type == 'x' ? "0x" : "0X";
        break;
    default:
        break;
    }
    char digits[64];
    const char* const end = to_chars(digits, digits + sizeof(digits), magnitude, base).ptr;
    if (spec.type == 'X')
    {
        for (char* digit = digits; digit != end; ++digit)
        {
            if (*digit >= 'a' && *digit <= 'f')
            {
                *digit = static_cast<char>(*digit - 'a' + 'A');
            }
        }
    }
    char prefix[3];
    size_t prefix_length = PutSign(prefix, spec, negative);
    if (spec.alternate)
    {
        for (const char* letter = base_prefix; *letter != '\0'; ++letter)
        {
            prefix[prefix_length++] = *letter;
        }
    }
    WriteNumber(out, spec,
                {string_view(prefix, prefix_length),
                 string_view(digits, static_cast<size_t>(end - digits)), 0, string_view()},
                true);
}

template <class T> const FloatLayout& LayoutOf()
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

bool IsUpperFloatType(char type)
{
    return type == 'A' || type == 'E' || type == 'F' || type == 'G';
}

// The sign comes from the bits, so no floating-point arithmetic is needed, which AArch64 lacks
// for long double.
template <class T> void WriteFloat(FormatBuffer& out, const FormatSpec& spec, T value)
{
    DecodedFloat decoded = Decode(LayoutOf<T>(), RawBits(value));
    char sign[1];
    const string_view prefix(sign, PutSign(sign, spec, decoded.negative));
    const bool upper = IsUpperFloatType(spec.type);
    if (decoded.kind == FloatKind::Infinite || decoded.kind == FloatKind::NotANumber)
    {
        const bool infinite = decoded.kind == FloatKind::Infinite;
        WriteNumber(out, spec,
                    {prefix, infinite ? (upper ? "INF" : "inf") : (upper ? "NAN" : "nan"), 0,
                     string_view()},
                    false);
        return;
    }
    if (spec.type == '\0' && spec.precision < 0)
    {
        // The shortest text that reads back as the value, with room for a point.
        char text[128];
        char* end = to_chars(text, text + sizeof(text) - 1, value).ptr;
        char* const digits = text + (decoded.negative ? 1 : 0);
        char* exponent = digits;
        bool point = false;
        for (; exponent != end && *exponent != 'e'; ++exponent)
        {
            point = point || *exponent == '.';
        }
        if (spec.alternate && !point)
        {
            for (char* moved = end; moved != exponent; --moved)
            {
                *moved = moved[-1];
            }
            *exponent = '.';
            ++end;
        }
        WriteNumber(
            out, spec,
            {prefix, string_view(digits, static_cast<size_t>(end - digits)), 0, string_view()},
            true);
        return;
    }
    decoded.negative = false;
    FloatOptions options = {spec.type == '\0' ? 'g' : spec.type, spec.precision, false, false,
                            spec.alternate && spec.type != '\0'};
    FloatText text;
    FeatherOS::LibC::FormatFloat(decoded, options, &text);
    bool point = false;
    for (size_t index = 0; index != text.body_length; ++index)
    {
        point = point || text.body[index] == '.';
    }
    if (spec.type == '\0' && spec.alternate && !point)
    {
        text.body[text.body_length++] = '.';
    }
    WriteNumber(out, spec,
                {prefix, string_view(text.body, text.body_length), text.trailing_zeros,
                 string_view(text.suffix, text.suffix_length)},
                true);
}

template <class T> void FormatFloat(format_context& context, const FormatSpec& spec, T value)
{
    WriteFloat(BufferOf(context), Resolve(context, spec), value);
}

class StringFormatBuffer final : public FormatBuffer
{
public:
    explicit StringFormatBuffer(string& target)
        : FormatBuffer(m_storage, sizeof(m_storage), &DrainTo), m_target(target)
    {
    }

private:
    static void DrainTo(FormatBuffer& buffer, const char* data, size_t size)
    {
        static_cast<StringFormatBuffer&>(buffer).m_target.append(data, size);
    }

    char m_storage[256];
    string& m_target;
};

class CountingFormatBuffer final : public FormatBuffer
{
public:
    CountingFormatBuffer() : FormatBuffer(m_storage, sizeof(m_storage), &DrainTo) {}

    size_t Count() const
    {
        return m_count;
    }

private:
    static void DrainTo(FormatBuffer& buffer, const char*, size_t size)
    {
        static_cast<CountingFormatBuffer&>(buffer).m_count += size;
    }

    char m_storage[128];
    size_t m_count = 0;
};

void WriteEscapedPadded(FormatBuffer& out, const FormatSpec& spec, string_view value, char quote)
{
    if (spec.width == 0 && spec.precision < 0)
    {
        WriteEscaped(out, value, quote);
        return;
    }
    string text;
    StringFormatBuffer escaped(text);
    WriteEscaped(escaped, value, quote);
    escaped.Flush();
    const Measured measured =
        Measure(text, spec.precision < 0 ? numeric_limits<size_t>::max()
                                         : static_cast<size_t>(spec.precision));
    WritePadded(out, spec, string_view(text).substr(0, measured.bytes), measured.width,
                FormatAlign::Left);
}

class FormatRunner
{
public:
    explicit FormatRunner(format_context& context) : m_context(context) {}

    void Text(const char* first, const char* last)
    {
        BufferOf(m_context).Write(first, static_cast<size_t>(last - first));
    }

    void Field(size_t id, basic_format_parse_context<char>& parse)
    {
        const basic_format_arg<format_context> argument = m_context.arg(id);
        if (!argument)
        {
            FormatError("format string refers to an argument that is not given");
        }
        visit_format_arg(
            [&](auto value)
            {
                using T = decltype(value);
                if constexpr (is_same_v<T, typename basic_format_arg<format_context>::handle>)
                {
                    value.format(parse, m_context);
                }
                else if constexpr (!is_same_v<T, monostate>)
                {
                    formatter<T, char> formatter;
                    parse.advance_to(formatter.parse(parse));
                    m_context.advance_to(formatter.format(value, m_context));
                }
            },
            argument);
    }

private:
    format_context& m_context;
};

} // namespace

void FormatIntegerValue(format_context& context, const FormatSpec& spec,
                        unsigned long long magnitude, bool negative)
{
    WriteInteger(BufferOf(context), Resolve(context, spec), magnitude, negative);
}

void FormatCharValue(format_context& context, const FormatSpec& spec, char value)
{
    const FormatSpec resolved = Resolve(context, spec);
    if (IsIntegerPresentation(resolved.type))
    {
        WriteInteger(BufferOf(context), resolved, static_cast<unsigned char>(value), false);
        return;
    }
    if (resolved.type == '?')
    {
        WriteEscapedPadded(BufferOf(context), resolved, string_view(&value, 1), '\'');
        return;
    }
    WriteCharacter(BufferOf(context), resolved, value);
}

void FormatBoolValue(format_context& context, const FormatSpec& spec, bool value)
{
    const FormatSpec resolved = Resolve(context, spec);
    if (resolved.type == 'c')
    {
        WriteCharacter(BufferOf(context), resolved, static_cast<char>(value));
    }
    else if (IsIntegerPresentation(resolved.type))
    {
        WriteInteger(BufferOf(context), resolved, value ? 1 : 0, false);
    }
    else
    {
        const string_view text = value ? "true" : "false";
        WritePadded(BufferOf(context), resolved, text, text.size(), FormatAlign::Left);
    }
}

void FormatStringValue(format_context& context, const FormatSpec& spec, string_view value)
{
    const FormatSpec resolved = Resolve(context, spec);
    if (resolved.type == '?')
    {
        WriteEscapedPadded(BufferOf(context), resolved, value, '"');
        return;
    }
    if (resolved.width == 0 && resolved.precision < 0)
    {
        BufferOf(context).Write(value);
        return;
    }
    const Measured measured =
        Measure(value, resolved.precision < 0 ? numeric_limits<size_t>::max()
                                              : static_cast<size_t>(resolved.precision));
    WritePadded(BufferOf(context), resolved, value.substr(0, measured.bytes), measured.width,
                FormatAlign::Left);
}

void FormatPointerValue(format_context& context, const FormatSpec& spec, const void* value)
{
    const FormatSpec resolved = Resolve(context, spec);
    char digits[2 * sizeof(void*)];
    char* const end =
        to_chars(digits, digits + sizeof(digits), reinterpret_cast<__UINTPTR_TYPE__>(value), 16)
            .ptr;
    if (resolved.type == 'P')
    {
        for (char* digit = digits; digit != end; ++digit)
        {
            if (*digit >= 'a' && *digit <= 'f')
            {
                *digit = static_cast<char>(*digit - 'a' + 'A');
            }
        }
    }
    WriteNumber(BufferOf(context), resolved,
                {resolved.type == 'P' ? "0X" : "0x",
                 string_view(digits, static_cast<size_t>(end - digits)), 0, string_view()},
                true);
}

void FormatFloatValue(format_context& context, const FormatSpec& spec, float value)
{
    FormatFloat(context, spec, value);
}

void FormatFloatValue(format_context& context, const FormatSpec& spec, double value)
{
    FormatFloat(context, spec, value);
}

void FormatFloatValue(format_context& context, const FormatSpec& spec, long double value)
{
    FormatFloat(context, spec, value);
}

void FormatPadded(format_context& context, const FormatSpec& spec, FormatBody body,
                  const void* state)
{
    const FormatSpec resolved = Resolve(context, spec);
    if (resolved.width == 0)
    {
        body(context, state);
        return;
    }
    string text;
    StringFormatBuffer buffer(text);
    format_context inner = FormatContextAccess::Make(buffer, FormatContextAccess::Args(context));
    body(inner, state);
    buffer.Flush();
    const Measured measured = Measure(text, numeric_limits<size_t>::max());
    WritePadded(BufferOf(context), resolved, text, measured.width, FormatAlign::Left);
}

void VFormat(FormatBuffer& buffer, string_view pattern, format_args args)
{
    format_context context = FormatContextAccess::Make(buffer, args);
    basic_format_parse_context<char> parse(pattern);
    FormatRunner runner(context);
    ScanFormatString(parse, runner);
}

size_t VFormattedSize(string_view pattern, format_args args)
{
    CountingFormatBuffer buffer;
    VFormat(buffer, pattern, args);
    buffer.Flush();
    return buffer.Count();
}

} // namespace __detail

string vformat(string_view pattern, format_args args)
{
    string result;
    __detail::StringFormatBuffer buffer(result);
    __detail::VFormat(buffer, pattern, args);
    buffer.Flush();
    return result;
}

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
