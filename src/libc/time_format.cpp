#include "time_format.h"

#include <stddef.h>

namespace FeatherOS::LibC
{

namespace TimeFormatDetail
{

Output::Output(char* target, size_t target_size) : buffer(target), size(target_size) {}

void Output::Put(char character)
{
    if (length + 1 < size)
    {
        buffer[length] = character;
    }
    else
    {
        overflowed = true;
    }
    ++length;
}

void Output::Text(const char* text, size_t count)
{
    for (size_t index = 0; index < count && text[index] != '\0'; ++index)
    {
        Put(text[index]);
    }
}

void Output::Number(long value, int width, char pad)
{
    char digits[24];
    int count = 0;
    const bool negative = value < 0;
    unsigned long magnitude =
        negative ? 0UL - static_cast<unsigned long>(value) : static_cast<unsigned long>(value);
    do
    {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative)
    {
        Put('-');
    }
    for (int padding = count; padding < width; ++padding)
    {
        Put(pad);
    }
    while (count > 0)
    {
        Put(digits[--count]);
    }
}

size_t Output::Finish()
{
    if (size == 0)
    {
        return 0;
    }
    if (overflowed)
    {
        buffer[0] = '\0';
        return 0;
    }
    buffer[length] = '\0';
    return length;
}

} // namespace TimeFormatDetail
} // namespace FeatherOS::LibC
