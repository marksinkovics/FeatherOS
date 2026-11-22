#include "text.h"

namespace FeatherOS::Usb
{

Text::Text(char* destination, size_t capacity) : out_(destination), capacity_(capacity) {}

void Text::Put(char value)
{
    if (length_ + 1 < capacity_)
    {
        out_[length_] = value;
    }
    ++length_;
}

void Text::Words(const char* text)
{
    while (*text != '\0')
    {
        Put(*text++);
    }
}

void Text::Hex(uint32_t value, uint32_t digits)
{
    constexpr char Digits[] = "0123456789abcdef";
    for (uint32_t shift = digits * 4; shift > 0; shift -= 4)
    {
        Put(Digits[(value >> (shift - 4)) & 0xFU]);
    }
}

void Text::Decimal(uint32_t value)
{
    char reversed[10];
    uint32_t count = 0;
    do
    {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0)
    {
        Put(reversed[--count]);
    }
}

bool Text::Finish()
{
    if (capacity_ == 0 || length_ + 1 > capacity_)
    {
        return false;
    }
    out_[length_] = '\0';
    return true;
}

} // namespace FeatherOS::Usb
