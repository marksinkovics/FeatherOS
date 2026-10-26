#ifndef FEATHEROS_LIBC_SCAN_H
#define FEATHEROS_LIBC_SCAN_H

#include "libc/float_parse.h"
#include "libc/integer_parse.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// The scanf engine, reading through a Source with int Get() (-1 at the end) and Unget(int),
// which only ever puts back the character just read. Returns the number of assignments, or
// -1 if the input ended before the first conversion.
template <typename Source> int ScanFrom(Source& source, const char* format, va_list arguments);

namespace ScanDetail
{

constexpr int End = -1;
// Longest integer text read when no width is given: a sign, a prefix, and 64 binary digits.
constexpr size_t MaximumNumberLength = 68;

template <typename Source> class Reader
{
public:
    explicit Reader(Source& input) : source(input) {}

    int Get()
    {
        const int character = source.Get();
        if (character != End)
        {
            ++consumed;
        }
        return character;
    }

    void Unget(int character)
    {
        if (character != End)
        {
            source.Unget(character);
            --consumed;
        }
    }

    // Skips white space; false if the input ended.
    bool SkipSpace()
    {
        int character = Get();
        while (IsSpace(static_cast<char>(character)) && character != End)
        {
            character = Get();
        }
        Unget(character);
        return character != End;
    }

    size_t Consumed() const
    {
        return consumed;
    }

private:
    Source& source;
    size_t consumed = 0;
};

bool InSet(const unsigned char* set, int character);

// Parses "[...]" after the '[' and returns the character after its ']', or nullptr.
const char* ParseSet(const char* format, unsigned char (&set)[32]);

// Reads the longest prefix that can begin an integer in base (0 picks it from the prefix,
// as strtol does) into text; false if no digit was read.
template <typename Source>
bool ReadInteger(Reader<Source>& reader, int base, size_t width, char (&text)[MaximumNumberLength],
                 int* actual_base)
{
    size_t length = 0;
    bool any_digit = false;
    int character = reader.Get();
    auto take = [&]()
    {
        text[length++] = static_cast<char>(character);
        character = reader.Get();
    };
    if ((character == '+' || character == '-') && length < width)
    {
        take();
    }
    if (character == '0' && length < width && (base == 0 || base == 16))
    {
        take();
        any_digit = true;
        if ((character == 'x' || character == 'X') && length < width)
        {
            take();
            base = 16;
            any_digit = false;
        }
        else if (base == 0)
        {
            base = 8;
        }
    }
    if (base == 0)
    {
        base = 10;
    }
    while (length < width && length + 1 < sizeof(text) && character != End &&
           DigitValue(static_cast<char>(character)) < static_cast<uint64_t>(base))
    {
        take();
        any_digit = true;
    }
    reader.Unget(character);
    text[length] = '\0';
    *actual_base = base;
    return any_digit;
}

constexpr size_t MaximumFloatLength = 512;

bool IsHexDigit(int character);
int LowerCase(int character);

// Reads the longest prefix of a strtod number into text: a sign, then "inf", "infinity",
// "nan", or hex or decimal digits with a point and an exponent. Since only one character can be
// put back, a word like "info" leaves its prefix read.
template <typename Source>
bool ReadFloatText(Reader<Source>& reader, size_t width, char (&text)[MaximumFloatLength])
{
    size_t length = 0;
    const size_t limit = width < sizeof(text) - 1 ? width : sizeof(text) - 1;
    int character = reader.Get();
    auto take = [&]()
    {
        text[length++] = static_cast<char>(character);
        character = reader.Get();
    };
    if ((character == '+' || character == '-') && length < limit)
    {
        take();
    }
    if (LowerCase(character) == 'i' || LowerCase(character) == 'n')
    {
        const char* word = LowerCase(character) == 'i' ? "infinity" : "nan";
        for (size_t index = 0;
             word[index] != '\0' && length < limit && LowerCase(character) == word[index]; ++index)
        {
            take();
        }
    }
    else
    {
        bool hex = false;
        if (character == '0' && length < limit)
        {
            take();
            if (LowerCase(character) == 'x' && length < limit)
            {
                take();
                hex = true;
            }
        }
        bool seen_point = false;
        while (length < limit)
        {
            const bool digit = hex ? IsHexDigit(character) : (character >= '0' && character <= '9');
            if (!digit && (character != '.' || seen_point))
            {
                break;
            }
            seen_point = seen_point || character == '.';
            take();
        }
        if (LowerCase(character) == (hex ? 'p' : 'e') && length < limit)
        {
            take();
            if ((character == '+' || character == '-') && length < limit)
            {
                take();
            }
            while (length < limit && character >= '0' && character <= '9')
            {
                take();
            }
        }
    }
    reader.Unget(character);
    text[length] = '\0';
    return length > 0;
}

template <typename Value> void Store(void* target, Value value)
{
    *static_cast<Value*>(target) = value;
}

void StoreInteger(char length, bool doubled, bool is_signed, uint64_t value, void* target);

} // namespace ScanDetail

template <typename Source> int ScanFrom(Source& source, const char* format, va_list arguments)
{
    using namespace ScanDetail;
    Reader<Source> reader(source);
    va_list list;
    va_copy(list, arguments);
    int assigned = 0;
    bool converted = false;
    // -1 when the input ends before the first conversion, as the standard requires.
    auto finish = [&](bool input_ended)
    {
        va_end(list);
        return input_ended && !converted ? -1 : assigned;
    };
    while (*format != '\0')
    {
        if (IsSpace(*format))
        {
            reader.SkipSpace();
            ++format;
            continue;
        }
        if (*format != '%' || format[1] == '%')
        {
            const char expected = *format == '%' ? '%' : *format;
            if (*format == '%')
            {
                reader.SkipSpace();
                ++format;
            }
            ++format;
            const int character = reader.Get();
            if (character != static_cast<unsigned char>(expected))
            {
                reader.Unget(character);
                return finish(character == End);
            }
            continue;
        }
        ++format;
        const bool suppress = *format == '*';
        if (suppress)
        {
            ++format;
        }
        size_t width = 0;
        for (; *format >= '0' && *format <= '9'; ++format)
        {
            width = width * 10 + static_cast<size_t>(*format - '0');
        }
        char length = 0;
        bool doubled = false;
        if (*format == 'h' || *format == 'l' || *format == 'j' || *format == 'z' ||
            *format == 't' || *format == 'L')
        {
            length = *format++;
            if ((length == 'h' || length == 'l') && *format == length)
            {
                doubled = true;
                ++format;
            }
        }
        const char conversion = *format++;
        switch (conversion)
        {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p':
        {
            if (!reader.SkipSpace())
            {
                return finish(true);
            }
            const int base = conversion == 'd' || conversion == 'u' ? 10
                             : conversion == 'i'                    ? 0
                             : conversion == 'o'                    ? 8
                                                                    : 16;
            char text[MaximumNumberLength];
            int actual_base = base;
            if (!ReadInteger(reader, base, width == 0 ? sizeof(text) - 1 : width, text,
                             &actual_base))
            {
                return finish(false);
            }
            converted = true;
            const ParsedInteger parsed = ParseInteger(text, actual_base, UINT64_MAX, UINT64_MAX);
            const uint64_t value = parsed.negative ? 0 - parsed.magnitude : parsed.magnitude;
            if (!suppress)
            {
                void* target = va_arg(list, void*);
                if (conversion == 'p')
                {
                    Store(target, reinterpret_cast<void*>(static_cast<uintptr_t>(value)));
                }
                else
                {
                    StoreInteger(length, doubled, conversion == 'd' || conversion == 'i', value,
                                 target);
                }
                ++assigned;
            }
            break;
        }
        case 'c':
        {
            char* target = suppress ? nullptr : va_arg(list, char*);
            const size_t count = width == 0 ? 1 : width;
            for (size_t index = 0; index < count; ++index)
            {
                const int character = reader.Get();
                if (character == End)
                {
                    return finish(index == 0);
                }
                if (target != nullptr)
                {
                    target[index] = static_cast<char>(character);
                }
            }
            converted = true;
            assigned += suppress ? 0 : 1;
            break;
        }
        case 's':
        case '[':
        {
            unsigned char set[32];
            if (conversion == '[')
            {
                format = ParseSet(format, set);
                if (format == nullptr)
                {
                    return finish(false);
                }
            }
            else if (!reader.SkipSpace())
            {
                return finish(true);
            }
            char* target = suppress ? nullptr : va_arg(list, char*);
            size_t count = 0;
            int character = reader.Get();
            while (character != End && (width == 0 || count < width) &&
                   (conversion == '[' ? InSet(set, character)
                                      : !IsSpace(static_cast<char>(character))))
            {
                if (target != nullptr)
                {
                    target[count] = static_cast<char>(character);
                }
                ++count;
                character = reader.Get();
            }
            reader.Unget(character);
            if (count == 0)
            {
                return finish(character == End);
            }
            if (target != nullptr)
            {
                target[count] = '\0';
                ++assigned;
            }
            converted = true;
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
        {
            if (!reader.SkipSpace())
            {
                return finish(true);
            }
            char text[MaximumFloatLength];
            if (!ReadFloatText(reader, width == 0 ? sizeof(text) - 1 : width, text))
            {
                return finish(false);
            }
            // No length means float, l double, and L long double.
            const FloatLayout& layout = length == 'L'   ? LongDoubleFormat
                                        : length == 'l' ? DoubleFormat
                                                        : FloatFormat;
            const ParsedFloat parsed = ParseFloat(layout, text);
            if (parsed.end == text)
            {
                return finish(false);
            }
            converted = true;
            if (!suppress)
            {
                void* target = va_arg(list, void*);
                if (length == 'L')
                {
                    Store(target, FromRawBits<long double>(parsed.bits));
                }
                else if (length == 'l')
                {
                    Store(target, FromRawBits<double>(parsed.bits));
                }
                else
                {
                    Store(target, FromRawBits<float>(parsed.bits));
                }
                ++assigned;
            }
            break;
        }
        case 'n':
            if (!suppress)
            {
                StoreInteger(length, doubled, true, reader.Consumed(), va_arg(list, void*));
            }
            break;
        default:
            // An unknown conversion, or the end of the format.
            return finish(false);
        }
    }
    return finish(false);
}

} // namespace FeatherOS::LibC

#endif
