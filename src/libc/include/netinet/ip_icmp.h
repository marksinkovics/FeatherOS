#ifndef FEATHEROS_LIBC_NETINET_IP_ICMP_H
#define FEATHEROS_LIBC_NETINET_IP_ICMP_H

#include <stdint.h>

// Linux's layout and names; fields are in network byte order.
struct icmphdr
{
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    union
    {
        struct
        {
            uint16_t id;
            uint16_t sequence;
        } echo;
        uint32_t gateway;
    } un;
};

#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO 8

#endif
