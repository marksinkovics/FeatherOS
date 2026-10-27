#include "format.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

namespace FeatherOS::String
{

FormatBuffer::FormatBuffer(char* output, size_t output_capacity)
    : destination(output), capacity(output_capacity), length(0),
      good(output != nullptr && output_capacity != 0)
{
    if (good)
    {
        destination[0] = '\0';
    }
}

bool FormatBuffer::AppendCharacter(char value)
{
    if (!good || length >= capacity - 1)
    {
        good = false;
        return false;
    }
    destination[length++] = value;
    destination[length] = '\0';
    return true;
}

bool FormatBuffer::AppendUnsigned(uint64_t value, uint32_t base)
{
    static constexpr char digits[] = "0123456789abcdef";
    char reversed[64];
    size_t digit_count = 0;
    do
    {
        reversed[digit_count++] = digits[value % base];
        value /= base;
    } while (value != 0);

    while (digit_count != 0)
    {
        if (!AppendCharacter(reversed[--digit_count]))
        {
            return false;
        }
    }
    return true;
}

bool FormatBuffer::AppendSigned(int64_t value)
{
    uint64_t magnitude = static_cast<uint64_t>(value);
    if (value < 0)
    {
        if (!AppendCharacter('-'))
        {
            return false;
        }
        magnitude = static_cast<uint64_t>(-(value + 1)) + 1;
    }
    return AppendUnsigned(magnitude, 10);
}

bool FormatBuffer::Append(bool value)
{
    return Append(value ? "true" : "false");
}

bool FormatBuffer::Append(char value)
{
    return AppendCharacter(value);
}

bool FormatBuffer::Append(const char* value)
{
    if (value == nullptr)
    {
        value = "(null)";
    }
    while (*value != '\0')
    {
        if (!AppendCharacter(*value++))
        {
            return false;
        }
    }
    return true;
}

bool FormatBuffer::Append(signed char value)
{
    return AppendSigned(value);
}

bool FormatBuffer::Append(unsigned char value)
{
    return AppendUnsigned(value, 10);
}

bool FormatBuffer::Append(short value)
{
    return AppendSigned(value);
}

bool FormatBuffer::Append(unsigned short value)
{
    return AppendUnsigned(value, 10);
}

bool FormatBuffer::Append(int value)
{
    return AppendSigned(value);
}

bool FormatBuffer::Append(unsigned int value)
{
    return AppendUnsigned(value, 10);
}

bool FormatBuffer::Append(long value)
{
    return AppendSigned(value);
}

bool FormatBuffer::Append(unsigned long value)
{
    return AppendUnsigned(value, 10);
}

bool FormatBuffer::Append(long long value)
{
    return AppendSigned(value);
}

bool FormatBuffer::Append(unsigned long long value)
{
    return AppendUnsigned(value, 10);
}

bool FormatBuffer::Append(const void* value)
{
    if (!Append("0x"))
    {
        return false;
    }
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(value);
    char reversed[sizeof(uintptr_t) * 2];
    size_t digit_count = 0;
    uintptr_t remaining = pointer;
    do
    {
        const uint8_t digit = static_cast<uint8_t>(remaining & 0xF);
        reversed[digit_count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        remaining >>= 4;
    } while (remaining != 0);
    while (digit_count != 0)
    {
        if (!AppendCharacter(reversed[--digit_count]))
        {
            return false;
        }
    }
    return true;
}

bool FormatBuffer::Good() const
{
    return good;
}

} // namespace FeatherOS::String

namespace FeatherOS::String
{

bool Consume(FormatBuffer& output, const char*& format)
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

} // namespace FeatherOS::String
