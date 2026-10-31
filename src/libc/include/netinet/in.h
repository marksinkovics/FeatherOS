#ifndef FEATHEROS_LIBC_NETINET_IN_H
#define FEATHEROS_LIBC_NETINET_IN_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

// Addresses and ports are in network byte order.
struct in_addr
{
    in_addr_t s_addr;
};

struct sockaddr_in
{
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct in6_addr
{
    uint8_t s6_addr[16];
};

struct sockaddr_in6
{
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58

// In host byte order, for htonl.
#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_LOOPBACK ((in_addr_t)0x7F000001)

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

#define IN6ADDR_ANY_INIT {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define IN6ADDR_LOOPBACK_INIT {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}

__BEGIN_DECLS

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

uint32_t htonl(uint32_t value);
uint16_t htons(uint16_t value);
uint32_t ntohl(uint32_t value);
uint16_t ntohs(uint16_t value);

__END_DECLS

#endif
