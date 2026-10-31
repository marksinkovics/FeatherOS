#include "address.h"

namespace FeatherOS::Network
{

bool operator==(const MacAddress& left, const MacAddress& right)
{
    for (size_t index = 0; index < sizeof(left.bytes); ++index)
    {
        if (left.bytes[index] != right.bytes[index])
        {
            return false;
        }
    }
    return true;
}

bool operator==(const IpAddress& left, const IpAddress& right)
{
    if (left.version != right.version)
    {
        return false;
    }
    for (size_t index = 0; index < sizeof(left.bytes); ++index)
    {
        if (left.bytes[index] != right.bytes[index])
        {
            return false;
        }
    }
    return true;
}

IpAddress MakeIpAddress(Ipv4Address address)
{
    IpAddress result = {.version = IpVersion::V4, .bytes = {}};
    WriteBig32(result.bytes, address);
    return result;
}

Ipv4Address ToIpv4(const IpAddress& address)
{
    return address.version == IpVersion::V4 ? ReadBig32(address.bytes) : 0;
}

bool IsUnspecified(const IpAddress& address)
{
    for (size_t index = 0; index < sizeof(address.bytes); ++index)
    {
        if (address.bytes[index] != 0)
        {
            return false;
        }
    }
    return address.version != IpVersion::None;
}

uint32_t PrefixLength(Ipv4Address netmask)
{
    uint32_t length = 0;
    while (length < 32 && (netmask & (0x80000000U >> length)) != 0)
    {
        ++length;
    }
    return length;
}

Ipv4Address PrefixNetmask(uint32_t length)
{
    // A shift by 32 is undefined.
    return length == 0 ? 0 : 0xFFFFFFFFU << (32 - (length > 32 ? 32 : length));
}

void FormatMac(const MacAddress& address, char (&text)[FormattedMacLength])
{
    constexpr char Digits[] = "0123456789abcdef";
    for (size_t index = 0; index < sizeof(address.bytes); ++index)
    {
        text[index * 3] = Digits[address.bytes[index] >> 4];
        text[index * 3 + 1] = Digits[address.bytes[index] & 0xF];
        text[index * 3 + 2] = index + 1 == sizeof(address.bytes) ? '\0' : ':';
    }
}

void FormatIpv4(Ipv4Address address, char (&text)[FormattedIpv4Length])
{
    size_t length = 0;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const uint32_t part = (address >> shift) & 0xFF;
        if (part >= 100)
        {
            text[length++] = static_cast<char>('0' + part / 100);
        }
        if (part >= 10)
        {
            text[length++] = static_cast<char>('0' + part / 10 % 10);
        }
        text[length++] = static_cast<char>('0' + part % 10);
        text[length++] = shift == 0 ? '\0' : '.';
    }
}

void FormatIpv6(const IpAddress& address, char (&text)[FormattedIpv6Length])
{
    constexpr char Digits[] = "0123456789abcdef";
    constexpr int GroupCount = 8;
    uint16_t groups[GroupCount];
    for (int index = 0; index < GroupCount; ++index)
    {
        groups[index] = ReadBig16(address.bytes + index * 2);
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
    size_t length = 0;
    for (int index = 0; index < GroupCount; ++index)
    {
        if (index == run_start)
        {
            text[length++] = ':';
            text[length++] = ':';
            index += run_length - 1;
            continue;
        }
        if (index > 0 && index != run_start + run_length)
        {
            text[length++] = ':';
        }
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            const uint32_t digit = (groups[index] >> shift) & 0xF;
            if (digit != 0 || started || shift == 0)
            {
                text[length++] = Digits[digit];
                started = true;
            }
        }
    }
    text[length] = '\0';
}

uint16_t ReadBig16(const uint8_t* data)
{
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t ReadBig32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

void WriteBig16(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteBig32(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

} // namespace FeatherOS::Network
