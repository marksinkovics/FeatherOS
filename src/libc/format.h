#ifndef FEATHEROS_LIBC_FORMAT_H
#define FEATHEROS_LIBC_FORMAT_H

#include "libc/float_format.h"
#include "libc/include/bits/featheros_format.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// The printf engine, writing through any Sink with a Put(char) member. Returns the number of
// characters the format produces. %n is not supported, since it writes through a pointer the
// format supplies.
template <typename Sink> int FormatTo(Sink& sink, const char* format, va_list arguments);
// The same with featheros_format's tagged arguments.
template <typename Sink>
int FormatTo(Sink& sink, const char* format, const featheros_format_argument* arguments,
             size_t count);

// snprintf's sink: keeps what fits, always leaves room for the terminator, and counts the rest.
struct BufferSink
{
    char* buffer;
    size_t capacity;
    size_t length = 0;

    void Put(char character);
    void Terminate() const;
};

namespace FormatDetail
{

struct Specification
{
    bool left = false;
    bool plus = false;
    bool space = false;
    bool alternate = false;
    bool zero = false;
    int width = 0;
    // -1 when the format gives none.
    int precision = -1;
    char length = 0;
    bool doubled_length = false;
};

template <typename Sink> class Writer
{
public:
    explicit Writer(Sink& target) : sink(target) {}

    void Put(char character)
    {
        sink.Put(character);
        ++count;
    }

    void Repeat(char character, int times)
    {
        for (int index = 0; index < times; ++index)
        {
            Put(character);
        }
    }

    void Text(const char* text, size_t length)
    {
        for (size_t index = 0; index < length; ++index)
        {
            Put(text[index]);
        }
    }

    int Count() const
    {
        return count;
    }

private:
    Sink& sink;
    int count = 0;
};

size_t Length(const char* text, int maximum);

// Pads text, already converted, to the field width on whichever side the flags ask for.
template <typename Sink>
void PadAndWrite(Writer<Sink>& writer, const Specification& specification, const char* text,
                 size_t length)
{
    const int padding = specification.width > static_cast<int>(length)
                            ? specification.width - static_cast<int>(length)
                            : 0;
    if (!specification.left)
    {
        writer.Repeat(' ', padding);
    }
    writer.Text(text, length);
    if (specification.left)
    {
        writer.Repeat(' ', padding);
    }
}

template <typename Sink>
void WriteInteger(Writer<Sink>& writer, const Specification& specification, uint64_t magnitude,
                  bool negative, unsigned base, bool uppercase, bool is_signed)
{
    const char* digit_set = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[24];
    int digit_count = 0;
    // A precision of 0 prints no digits for the value 0.
    if (magnitude != 0 || specification.precision != 0)
    {
        do
        {
            digits[digit_count++] = digit_set[magnitude % base];
            magnitude /= base;
        } while (magnitude != 0);
    }

    char prefix[2];
    int prefix_length = 0;
    if (is_signed && negative)
    {
        prefix[prefix_length++] = '-';
    }
    else if (is_signed && specification.plus)
    {
        prefix[prefix_length++] = '+';
    }
    else if (is_signed && specification.space)
    {
        prefix[prefix_length++] = ' ';
    }
    else if (specification.alternate && base == 16 && digit_count > 0 &&
             !(digit_count == 1 && digits[0] == '0'))
    {
        prefix[prefix_length++] = '0';
        prefix[prefix_length++] = uppercase ? 'X' : 'x';
    }

    int zeros = specification.precision > digit_count ? specification.precision - digit_count : 0;
    // '#' with octal makes the first digit a 0.
    if (specification.alternate && base == 8 && zeros == 0 &&
        (digit_count == 0 || digits[digit_count - 1] != '0'))
    {
        zeros = 1;
    }
    int total = prefix_length + zeros + digit_count;
    // The 0 flag pads with zeros after the prefix, but a precision turns it off.
    if (specification.zero && !specification.left && specification.precision < 0 &&
        specification.width > total)
    {
        zeros += specification.width - total;
        total = specification.width;
    }
    const int padding = specification.width > total ? specification.width - total : 0;
    if (!specification.left)
    {
        writer.Repeat(' ', padding);
    }
    writer.Text(prefix, static_cast<size_t>(prefix_length));
    writer.Repeat('0', zeros);
    while (digit_count > 0)
    {
        writer.Put(digits[--digit_count]);
    }
    if (specification.left)
    {
        writer.Repeat(' ', padding);
    }
}

// The engine's arguments come from one of these. Each call takes the next argument, and returns
// false when it is missing or of another type.
class VariadicArguments
{
public:
    explicit VariadicArguments(va_list arguments);
    ~VariadicArguments();
    VariadicArguments(const VariadicArguments&) = delete;
    VariadicArguments& operator=(const VariadicArguments&) = delete;

    bool Int(int* value);
    bool Signed(const Specification& specification, int64_t* value);
    bool Unsigned(const Specification& specification, uint64_t* value);
    bool Character(char* value);
    bool String(const char** value);
    bool Pointer(const void** value);
    bool Float(const Specification& specification, DecodedFloat* value);

private:
    va_list list;
};

class TaggedArguments
{
public:
    TaggedArguments(const featheros_format_argument* arguments, size_t count);

    bool Int(int* value);
    bool Signed(const Specification& specification, int64_t* value);
    bool Unsigned(const Specification& specification, uint64_t* value);
    bool Character(char* value);
    bool String(const char** value);
    bool Pointer(const void** value);
    bool Float(const Specification& specification, DecodedFloat* value);

private:
    const featheros_format_argument* Take(int type);
    bool Integer(uint64_t* bits);

    const featheros_format_argument* arguments;
    size_t count;
    size_t next = 0;
};

template <typename Sink>
void WriteFloat(Writer<Sink>& writer, const Specification& specification, char conversion,
                const DecodedFloat& value)
{
    FloatText text;
    FormatFloat(value,
                {conversion, specification.precision, specification.plus, specification.space,
                 specification.alternate},
                &text);
    const size_t total =
        text.prefix_length + text.body_length + text.trailing_zeros + text.suffix_length;
    const size_t width = specification.width > 0 ? static_cast<size_t>(specification.width) : 0;
    const size_t padding = width > total ? width - total : 0;
    // The 0 flag pads with zeros after the sign, except for infinity and NaN.
    const bool zero_pad = specification.zero && !specification.left && text.finite;
    if (!specification.left && !zero_pad)
    {
        writer.Repeat(' ', static_cast<int>(padding));
    }
    writer.Text(text.prefix, text.prefix_length);
    if (zero_pad)
    {
        writer.Repeat('0', static_cast<int>(padding));
    }
    writer.Text(text.body, text.body_length);
    writer.Repeat('0', static_cast<int>(text.trailing_zeros));
    writer.Text(text.suffix, text.suffix_length);
    if (specification.left)
    {
        writer.Repeat(' ', static_cast<int>(padding));
    }
}

bool IsDigit(char character);

} // namespace FormatDetail

namespace FormatDetail
{

template <typename Sink, typename Arguments>
int Format(Sink& sink, const char* format, Arguments& arguments)
{
    Writer<Sink> writer(sink);
    while (*format != '\0')
    {
        if (*format != '%')
        {
            writer.Put(*format++);
            continue;
        }
        const char* start = format++;
        Specification specification;
        // Set when a * has no suitable argument, so that the whole specification is written as is.
        bool missing = false;
        for (;; ++format)
        {
            if (*format == '-')
            {
                specification.left = true;
            }
            else if (*format == '+')
            {
                specification.plus = true;
            }
            else if (*format == ' ')
            {
                specification.space = true;
            }
            else if (*format == '#')
            {
                specification.alternate = true;
            }
            else if (*format == '0')
            {
                specification.zero = true;
            }
            else
            {
                break;
            }
        }
        if (*format == '*')
        {
            missing = !arguments.Int(&specification.width);
            // A negative width from an argument means left alignment.
            if (specification.width < 0)
            {
                specification.left = true;
                specification.width = -specification.width;
            }
            ++format;
        }
        for (; IsDigit(*format); ++format)
        {
            specification.width = specification.width * 10 + (*format - '0');
        }
        if (*format == '.')
        {
            ++format;
            specification.precision = 0;
            if (*format == '*')
            {
                missing = !arguments.Int(&specification.precision) || missing;
                ++format;
            }
            for (; IsDigit(*format); ++format)
            {
                specification.precision = specification.precision * 10 + (*format - '0');
            }
            // A negative precision from an argument counts as none.
            if (specification.precision < 0)
            {
                specification.precision = -1;
            }
        }
        if (*format == 'h' || *format == 'l' || *format == 'j' || *format == 'z' ||
            *format == 't' || *format == 'L')
        {
            specification.length = *format;
            ++format;
            if ((specification.length == 'h' || specification.length == 'l') &&
                *format == specification.length)
            {
                specification.doubled_length = true;
                ++format;
            }
        }

        const char conversion = *format;
        if (conversion == '\0')
        {
            writer.Text(start, static_cast<size_t>(format - start));
            break;
        }
        ++format;
        // The argument's text when it is missing or unsuitable.
        const auto as_written = [&] { writer.Text(start, static_cast<size_t>(format - start)); };
        switch (conversion)
        {
        case 'd':
        case 'i':
        {
            int64_t value = 0;
            if (!arguments.Signed(specification, &value) || missing)
            {
                as_written();
                break;
            }
            const uint64_t magnitude =
                value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
            WriteInteger(writer, specification, magnitude, value < 0, 10, false, true);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        {
            uint64_t value = 0;
            if (!arguments.Unsigned(specification, &value) || missing)
            {
                as_written();
                break;
            }
            const unsigned base = conversion == 'u' ? 10 : (conversion == 'o' ? 8 : 16);
            WriteInteger(writer, specification, value, false, base, conversion == 'X', false);
            break;
        }
        case 'p':
        {
            const void* pointer = nullptr;
            if (!arguments.Pointer(&pointer) || missing)
            {
                as_written();
                break;
            }
            if (pointer == nullptr)
            {
                PadAndWrite(writer, specification, "(nil)", 5);
                break;
            }
            specification.alternate = true;
            specification.zero = false;
            WriteInteger(writer, specification, reinterpret_cast<uintptr_t>(pointer), false, 16,
                         false, false);
            break;
        }
        case 'c':
        {
            char character = 0;
            if (!arguments.Character(&character) || missing)
            {
                as_written();
                break;
            }
            PadAndWrite(writer, specification, &character, 1);
            break;
        }
        case 's':
        {
            const char* text = nullptr;
            if (!arguments.String(&text) || missing)
            {
                as_written();
                break;
            }
            if (text == nullptr)
            {
                text = "(null)";
            }
            PadAndWrite(writer, specification, text, Length(text, specification.precision));
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
        {
            DecodedFloat value;
            if (!arguments.Float(specification, &value) || missing)
            {
                as_written();
                break;
            }
            WriteFloat(writer, specification, conversion, value);
            break;
        }
        case '%':
            writer.Put('%');
            break;
        default:
            // Unsupported conversions are printed as written.
            as_written();
            break;
        }
    }
    return writer.Count();
}

} // namespace FormatDetail

template <typename Sink> int FormatTo(Sink& sink, const char* format, va_list arguments)
{
    FormatDetail::VariadicArguments source(arguments);
    return FormatDetail::Format(sink, format, source);
}

template <typename Sink>
int FormatTo(Sink& sink, const char* format, const featheros_format_argument* arguments,
             size_t count)
{
    FormatDetail::TaggedArguments source(arguments, count);
    return FormatDetail::Format(sink, format, source);
}

} // namespace FeatherOS::LibC

#endif
