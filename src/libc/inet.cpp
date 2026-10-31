#include "libc/address_text.h"

#include <arpa/inet.h>
#include <errno.h>

uint32_t htonl(uint32_t value)
{
    return ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) | ((value >> 8) & 0xFF00) |
           (value >> 24);
}

uint16_t htons(uint16_t value)
{
    return static_cast<uint16_t>((value << 8) | (value >> 8));
}

uint32_t ntohl(uint32_t value)
{
    return htonl(value);
}

uint16_t ntohs(uint16_t value)
{
    return htons(value);
}

const in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

int inet_pton(int family, const char* text, void* address)
{
    if (family == AF_INET6)
    {
        uint8_t parsed[16];
        if (!FeatherOS::LibC::ParseIpv6Text(text, parsed))
        {
            return 0;
        }
        auto* bytes = static_cast<uint8_t*>(address);
        for (int index = 0; index < 16; ++index)
        {
            bytes[index] = parsed[index];
        }
        return 1;
    }
    if (family != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    uint8_t parsed[4];
    if (!FeatherOS::LibC::ParseIpv4Text(text, parsed))
    {
        return 0;
    }
    auto* bytes = static_cast<uint8_t*>(address);
    for (int index = 0; index < 4; ++index)
    {
        bytes[index] = parsed[index];
    }
    return 1;
}

const char* inet_ntop(int family, const void* address, char* text, socklen_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(address);
    if (family == AF_INET6)
    {
        uint8_t parts[16];
        for (int index = 0; index < 16; ++index)
        {
            parts[index] = bytes[index];
        }
        if (FeatherOS::LibC::FormatIpv6Text(parts, text, size) == 0)
        {
            errno = ENOSPC;
            return nullptr;
        }
        return text;
    }
    if (family != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return nullptr;
    }
    const uint8_t parts[4] = {bytes[0], bytes[1], bytes[2], bytes[3]};
    if (FeatherOS::LibC::FormatIpv4Text(parts, text, size) == 0)
    {
        errno = ENOSPC;
        return nullptr;
    }
    return text;
}
