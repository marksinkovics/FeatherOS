#ifndef FEATHEROS_STRING_FORMAT_H
#define FEATHEROS_STRING_FORMAT_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::String
{

class FormatBuffer
{
public:
    FormatBuffer(char* destination, size_t capacity);

    bool Append(bool value);
    bool Append(char value);
    bool Append(const char* value);
    bool Append(signed char value);
    bool Append(unsigned char value);
    bool Append(short value);
    bool Append(unsigned short value);
    bool Append(int value);
    bool Append(unsigned int value);
    bool Append(long value);
    bool Append(unsigned long value);
    bool Append(long long value);
    bool Append(unsigned long long value);
    bool Append(const void* value);

    bool AppendCharacter(char value);
    bool Good() const;

private:
    bool AppendSigned(int64_t value);
    bool AppendUnsigned(uint64_t value, uint32_t base);

    char* destination;
    size_t capacity;
    size_t length;
    bool good;
};

inline bool Consume(FormatBuffer& output, const char*& format)
{
    while (*format != '\0')
    {
        if (format[0] == '{' && format[1] == '{')
        {
            if (!output.AppendCharacter('{'))
            {
                return false;
            }
            format += 2;
        }
        else if (format[0] == '}' && format[1] == '}')
        {
            if (!output.AppendCharacter('}'))
            {
                return false;
            }
            format += 2;
        }
        else if (*format == '{' || *format == '}')
        {
            return false;
        }
        else if (!output.AppendCharacter(*format++))
        {
            return false;
        }
    }
    return true;
}

template <typename Value, typename... Rest>
bool Consume(FormatBuffer& output, const char*& format, const Value& value, const Rest&... rest)
{
    while (*format != '\0')
    {
        if (format[0] == '{' && format[1] == '{')
        {
            if (!output.AppendCharacter('{'))
            {
                return false;
            }
            format += 2;
        }
        else if (format[0] == '}' && format[1] == '}')
        {
            if (!output.AppendCharacter('}'))
            {
                return false;
            }
            format += 2;
        }
        else if (format[0] == '{' && format[1] == '}')
        {
            if (!output.Append(value))
            {
                return false;
            }
            format += 2;
            return Consume(output, format, rest...);
        }
        else if (*format == '{' || *format == '}')
        {
            return false;
        }
        else if (!output.AppendCharacter(*format++))
        {
            return false;
        }
    }
    return false;
}

template <typename... Args>
bool FormatTo(char* destination, size_t capacity, const char* format, const Args&... args)
{
    FormatBuffer output(destination, capacity);
    if (format == nullptr || !output.Good())
    {
        return false;
    }
    const char* cursor = format;
    return Consume(output, cursor, args...) && output.Good();
}

} // namespace FeatherOS::String

#endif
