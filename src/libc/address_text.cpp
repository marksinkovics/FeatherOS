#include "address_text.h"

namespace FeatherOS::LibC
{
namespace
{

int HexValue(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

bool ParseIpv4Text(const char* text, uint8_t (&address)[4])
{
    if (text == nullptr)
    {
        return false;
    }
    uint8_t parsed[4];
    const char* cursor = text;
    for (int part = 0; part < 4; ++part)
    {
        if (part > 0 && *cursor++ != '.')
        {
            return false;
        }
        unsigned value = 0;
        int digits = 0;
        while (*cursor >= '0' && *cursor <= '9')
        {
            if (digits == 3 || (digits == 1 && value == 0))
            {
                return false;
            }
            value = value * 10 + static_cast<unsigned>(*cursor - '0');
            ++digits;
            ++cursor;
        }
        if (digits == 0 || value > 255)
        {
            return false;
        }
        parsed[part] = static_cast<uint8_t>(value);
    }
    if (*cursor != '\0')
    {
        return false;
    }
    for (int part = 0; part < 4; ++part)
    {
        address[part] = parsed[part];
    }
    return true;
}

size_t FormatIpv4Text(const uint8_t (&address)[4], char* text, size_t capacity)
{
    char buffer[Ipv4TextCapacity];
    size_t length = 0;
    for (int part = 0; part < 4; ++part)
    {
        const unsigned value = address[part];
        if (part > 0)
        {
            buffer[length++] = '.';
        }
        if (value >= 100)
        {
            buffer[length++] = static_cast<char>('0' + value / 100);
        }
        if (value >= 10)
        {
            buffer[length++] = static_cast<char>('0' + value / 10 % 10);
        }
        buffer[length++] = static_cast<char>('0' + value % 10);
    }
    if (text == nullptr || length >= capacity)
    {
        return 0;
    }
    for (size_t index = 0; index < length; ++index)
    {
        text[index] = buffer[index];
    }
    text[length] = '\0';
    return length;
}

bool ParseIpv6Text(const char* text, uint8_t (&address)[16])
{
    if (text == nullptr)
    {
        return false;
    }
    uint8_t parsed[16] = {};
    int count = 0;
    // Where "::" stands, as a byte index into parsed, or -1.
    int gap = -1;
    const char* cursor = text;
    if (cursor[0] == ':')
    {
        if (cursor[1] != ':')
        {
            return false;
        }
        gap = 0;
        cursor += 2;
    }
    while (*cursor != '\0')
    {
        const char* end = cursor;
        while (HexValue(*end) >= 0)
        {
            ++end;
        }
        if (*end == '.')
        {
            uint8_t ipv4[4];
            if (count > 12 || !ParseIpv4Text(cursor, ipv4))
            {
                return false;
            }
            for (uint8_t part : ipv4)
            {
                parsed[count++] = part;
            }
            break;
        }
        const long digits = end - cursor;
        if (digits == 0 || digits > 4 || count == 16)
        {
            return false;
        }
        unsigned group = 0;
        for (; cursor < end; ++cursor)
        {
            group = group * 16 + static_cast<unsigned>(HexValue(*cursor));
        }
        parsed[count++] = static_cast<uint8_t>(group >> 8);
        parsed[count++] = static_cast<uint8_t>(group);
        if (*cursor == '\0')
        {
            break;
        }
        if (*cursor++ != ':')
        {
            return false;
        }
        if (*cursor == ':')
        {
            if (gap >= 0)
            {
                return false;
            }
            gap = count;
            ++cursor;
        }
        else if (*cursor == '\0')
        {
            return false;
        }
    }
    if (gap < 0 ? count != 16 : count > 14)
    {
        return false;
    }
    for (int index = 0; index < 16; ++index)
    {
        address[index] = 0;
    }
    const int tail = gap < 0 ? 0 : count - gap;
    const int head = count - tail;
    for (int index = 0; index < head; ++index)
    {
        address[index] = parsed[index];
    }
    for (int index = 0; index < tail; ++index)
    {
        address[16 - tail + index] = parsed[head + index];
    }
    return true;
}

size_t FormatIpv6Text(const uint8_t (&address)[16], char* text, size_t capacity)
{
    constexpr char Digits[] = "0123456789abcdef";
    constexpr int GroupCount = 8;
    unsigned groups[GroupCount];
    for (int index = 0; index < GroupCount; ++index)
    {
        groups[index] = (static_cast<unsigned>(address[index * 2]) << 8) | address[index * 2 + 1];
    }
    // RFC 5952: the longest run of two or more zero groups, the first of equal ones, becomes "::".
    int run_start = -1;
    int run_length = 1;
    for (int index = 0; index < GroupCount; ++index)
    {
        int end = index;
        while (end < GroupCount && groups[end] == 0)
        {
            ++end;
        }
        if (end - index > run_length)
        {
            run_start = index;
            run_length = end - index;
        }
        if (end > index)
        {
            index = end - 1;
        }
    }
    char buffer[Ipv6TextCapacity];
    size_t length = 0;
    for (int index = 0; index < GroupCount; ++index)
    {
        if (index == run_start)
        {
            buffer[length++] = ':';
            buffer[length++] = ':';
            index += run_length - 1;
            continue;
        }
        if (index > 0 && index != run_start + run_length)
        {
            buffer[length++] = ':';
        }
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            const unsigned digit = (groups[index] >> shift) & 0xF;
            if (digit != 0 || started || shift == 0)
            {
                buffer[length++] = Digits[digit];
                started = true;
            }
        }
    }
    if (text == nullptr || length >= capacity)
    {
        return 0;
    }
    for (size_t index = 0; index < length; ++index)
    {
        text[index] = buffer[index];
    }
    text[length] = '\0';
    return length;
}

size_t ParseAddressText(const char* text, size_t length, uint8_t (&address)[16])
{
    char copy[Ipv6TextCapacity];
    if (length == 0 || length >= sizeof(copy))
    {
        return 0;
    }
    for (size_t index = 0; index < length; ++index)
    {
        copy[index] = text[index];
    }
    copy[length] = '\0';
    uint8_t ipv4[4];
    if (ParseIpv4Text(copy, ipv4))
    {
        for (size_t index = 0; index < sizeof(ipv4); ++index)
        {
            address[index] = ipv4[index];
        }
        return sizeof(ipv4);
    }
    return ParseIpv6Text(copy, address) ? sizeof(address) : 0;
}

} // namespace FeatherOS::LibC
