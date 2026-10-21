#ifndef FEATHEROS_LIBC_FORMAT_H
#define FEATHEROS_LIBC_FORMAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// The printf engine, writing through any Sink with a Put(char) member. Returns the number of
// characters the format produces. Floating-point conversions (%f, %e, %g, %a) are printed as
// written and consume no argument until user programs can use floating point; %n is not
// supported, since it writes through a pointer the format supplies.
template <typename Sink> int FormatTo(Sink& sink, const char* format, va_list arguments);

// snprintf's sink: keeps what fits, always leaves room for the terminator, and counts the rest.
struct BufferSink
{
    char* buffer;
    size_t capacity;
    size_t length = 0;

    void Put(char character)
    {
        if (length + 1 < capacity)
        {
            buffer[length] = character;
        }
        ++length;
    }

    void Terminate() const
    {
        if (capacity > 0)
        {
            buffer[length < capacity ? length : capacity - 1] = '\0';
        }
    }
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

inline size_t Length(const char* text, int maximum)
{
    size_t length = 0;
    while ((maximum < 0 || length < static_cast<size_t>(maximum)) && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

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

inline int64_t SignedArgument(const Specification& specification, va_list* arguments)
{
    switch (specification.length)
    {
    case 'l':
        return specification.doubled_length ? va_arg(*arguments, long long)
                                            : va_arg(*arguments, long);
    case 'j':
        return va_arg(*arguments, intmax_t);
    case 'z':
    case 't':
        return va_arg(*arguments, ptrdiff_t);
    case 'h':
        return specification.doubled_length ? static_cast<signed char>(va_arg(*arguments, int))
                                            : static_cast<short>(va_arg(*arguments, int));
    default:
        return va_arg(*arguments, int);
    }
}

inline uint64_t UnsignedArgument(const Specification& specification, va_list* arguments)
{
    switch (specification.length)
    {
    case 'l':
        return specification.doubled_length ? va_arg(*arguments, unsigned long long)
                                            : va_arg(*arguments, unsigned long);
    case 'j':
        return va_arg(*arguments, uintmax_t);
    case 'z':
    case 't':
        return va_arg(*arguments, size_t);
    case 'h':
        return specification.doubled_length
                   ? static_cast<unsigned char>(va_arg(*arguments, unsigned int))
                   : static_cast<unsigned short>(va_arg(*arguments, unsigned int));
    default:
        return va_arg(*arguments, unsigned int);
    }
}

inline bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

} // namespace FormatDetail

template <typename Sink> int FormatTo(Sink& sink, const char* format, va_list arguments)
{
    using namespace FormatDetail;
    Writer<Sink> writer(sink);
    // Copied so that helpers can take its address, which is portable for every va_list type.
    va_list list;
    va_copy(list, arguments);
    while (*format != '\0')
    {
        if (*format != '%')
        {
            writer.Put(*format++);
            continue;
        }
        const char* start = format++;
        Specification specification;
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
            specification.width = va_arg(list, int);
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
                specification.precision = va_arg(list, int);
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
        switch (conversion)
        {
        case 'd':
        case 'i':
        {
            const int64_t value = SignedArgument(specification, &list);
            const uint64_t magnitude =
                value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
            WriteInteger(writer, specification, magnitude, value < 0, 10, false, true);
            break;
        }
        case 'u':
            WriteInteger(writer, specification, UnsignedArgument(specification, &list), false, 10,
                         false, false);
            break;
        case 'o':
            WriteInteger(writer, specification, UnsignedArgument(specification, &list), false, 8,
                         false, false);
            break;
        case 'x':
        case 'X':
            WriteInteger(writer, specification, UnsignedArgument(specification, &list), false, 16,
                         conversion == 'X', false);
            break;
        case 'p':
        {
            const void* pointer = va_arg(list, void*);
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
            const char character = static_cast<char>(va_arg(list, int));
            PadAndWrite(writer, specification, &character, 1);
            break;
        }
        case 's':
        {
            const char* text = va_arg(list, const char*);
            if (text == nullptr)
            {
                text = "(null)";
            }
            PadAndWrite(writer, specification, text, Length(text, specification.precision));
            break;
        }
        case '%':
            writer.Put('%');
            break;
        default:
            // Unsupported conversions are printed as written.
            writer.Text(start, static_cast<size_t>(format - start));
            break;
        }
    }
    va_end(list);
    return writer.Count();
}

} // namespace FeatherOS::LibC

#endif
