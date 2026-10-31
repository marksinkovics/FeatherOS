#ifndef FEATHEROS_LIBC_FEATHEROS_H
#define FEATHEROS_LIBC_FEATHEROS_H

#include <netinet/in.h>
#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>

// FeatherOS calls with no POSIX equivalent.

#define FEATHEROS_NAME_SERVERS 3
#define FEATHEROS_NETWORK_IPV4 0x1
#define FEATHEROS_NETWORK_NAME_SERVERS 0x2
#define FEATHEROS_NETWORK_LINK 0x4

// The network interface's settings. IPv4 is set by DHCP or by hand and is 0.0.0.0 until then;
// IPv6 comes from router advertisements, and missing addresses are all zero. The name servers
// set with FEATHEROS_NETWORK_NAME_SERVERS come first, then one a router advertised; each is a
// sockaddr_in or sockaddr_in6, and port 0 means 53. link_up is 0 while the link is down, when
// only loopback addresses are left.
struct featheros_network
{
    unsigned char mac[6];
    unsigned char ipv4_prefix_length;
    unsigned char ipv6_prefix_length;
    struct in_addr ipv4_address;
    struct in_addr ipv4_gateway;
    struct in6_addr ipv6_link_local;
    struct in6_addr ipv6_global;
    struct in6_addr ipv6_router;
    int link_up;
    unsigned int name_server_count;
    struct sockaddr_storage name_servers[FEATHEROS_NAME_SERVERS];
};

__BEGIN_DECLS

// Shows prompt and reads one console line with editing, without the newline. Ctrl+C discards
// the typed text and shows the prompt again, which printing the prompt and calling fgets cannot
// do. Flushes stdout first; input buffered in stdin is not seen. Returns the line's length, or
// -1 with errno (EIO when the caller is not in the foreground).
ssize_t featheros_read_line(const char* prompt, char* buffer, size_t size);
// The read-only page where the kernel placed the program's arguments and environment. The C
// library copies both out before main, so only its address is of interest, for example to see
// address-space layout randomization.
const void* featheros_arguments_page(void);
// Stores the current settings in old_settings, unless it is null, then applies the parts of
// new_settings that changes names (FEATHEROS_NETWORK_IPV4, FEATHEROS_NETWORK_NAME_SERVERS,
// FEATHEROS_NETWORK_LINK). Taking the link down resets the device, drops the addresses, the
// neighbor caches and every TCP connection off the machine; bringing it up starts IPv6
// autoconfiguration again, while IPv4 waits for DHCP or an address set by hand. Returns 0, or -1
// with errno: ENETUNREACH without a network device, EINVAL for a prefix over 32 or too many name
// servers, EAFNOSUPPORT for a name server of another family, ENETDOWN for IPv4 while the link is
// down, EIO when the device does not start again.
int featheros_network_settings(const struct featheros_network* new_settings,
                               struct featheros_network* old_settings, int changes);

__END_DECLS

#endif
