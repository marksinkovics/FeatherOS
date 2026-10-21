#ifndef FEATHEROS_LIBC_SCAN_H
#define FEATHEROS_LIBC_SCAN_H

#include "libc/integer_parse.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// The scanf engine, reading through a Source with int Get() (-1 at the end) and Unget(int),
// which only ever puts back the character just read. Returns the number of assignments, or
// -1 if the input ended before the first conversion. Floating-point conversions stop the scan
// until user programs can use floating point.
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

inline bool InSet(const unsigned char* set, int character)
{
    return (set[static_cast<unsigned char>(character) / 8] &
            (1U << (static_cast<unsigned char>(character) % 8))) != 0;
}

// Parses "[...]" after the '[' and returns the character after its ']', or nullptr.
inline const char* ParseSet(const char* format, unsigned char (&set)[32])
{
    bool negate = false;
    if (*format == '^')
    {
        negate = true;
        ++format;
    }
    for (unsigned char& bits : set)
    {
        bits = 0;
    }
    auto add = [&set](unsigned char character)
    {
        set[character / 8] =
            static_cast<unsigned char>(set[character / 8] | (1U << (character % 8)));
    };
    // A ']' first in the set is a member, not the end.
    if (*format == ']')
    {
        add(']');
        ++format;
    }
    for (; *format != '\0' && *format != ']'; ++format)
    {
        const auto first = static_cast<unsigned char>(*format);
        if (format[1] == '-' && format[2] != ']' && format[2] != '\0')
        {
            const auto last = static_cast<unsigned char>(format[2]);
            for (unsigned value = first; value <= last; ++value)
            {
                add(static_cast<unsigned char>(value));
            }
            format += 2;
            continue;
        }
        add(first);
    }
    if (*format != ']')
    {
        return nullptr;
    }
    if (negate)
    {
        for (unsigned char& bits : set)
        {
            bits = static_cast<unsigned char>(~bits);
        }
    }
    return format + 1;
}

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

template <typename Value> void Store(void* target, Value value)
{
    *static_cast<Value*>(target) = value;
}

inline void StoreInteger(char length, bool doubled, bool is_signed, uint64_t value, void* target)
{
    switch (length)
    {
    case 'h':
        if (doubled)
        {
            is_signed ? Store(target, static_cast<signed char>(value))
                      : Store(target, static_cast<unsigned char>(value));
        }
        else
        {
            is_signed ? Store(target, static_cast<short>(value))
                      : Store(target, static_cast<unsigned short>(value));
        }
        return;
    case 'l':
        if (doubled)
        {
            is_signed ? Store(target, static_cast<long long>(value))
                      : Store(target, static_cast<unsigned long long>(value));
        }
        else
        {
            is_signed ? Store(target, static_cast<long>(value))
                      : Store(target, static_cast<unsigned long>(value));
        }
        return;
    case 'j':
        is_signed ? Store(target, static_cast<intmax_t>(value))
                  : Store(target, static_cast<uintmax_t>(value));
        return;
    case 'z':
    case 't':
        is_signed ? Store(target, static_cast<ptrdiff_t>(value))
                  : Store(target, static_cast<size_t>(value));
        return;
    default:
        is_signed ? Store(target, static_cast<int>(value))
                  : Store(target, static_cast<unsigned int>(value));
        return;
    }
}

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
        case 'n':
            if (!suppress)
            {
                StoreInteger(length, doubled, true, reader.Consumed(), va_arg(list, void*));
            }
            break;
        default:
            // Floating point, an unknown conversion, or the end of the format.
            return finish(false);
        }
    }
    return finish(false);
}

} // namespace FeatherOS::LibC

#endif
