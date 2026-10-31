#ifndef FEATHEROS_USER_DHCP_MESSAGE_H
#define FEATHEROS_USER_DHCP_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

// DHCP client messages (RFC 2131, options from RFC 2132). Addresses are 4 bytes in network order.
namespace FeatherOS::User::Dhcp
{

// What every DHCP host must accept: 576 bytes, less the IP and UDP headers. Some servers, such as
// QEMU's, ignore shorter messages.
constexpr size_t MessageSize = 548;
constexpr uint16_t ServerPort = 67;
constexpr uint16_t ClientPort = 68;
constexpr uint8_t Discover = 1;
constexpr uint8_t Offer = 2;
constexpr uint8_t Request = 3;
constexpr uint8_t Ack = 5;
constexpr uint8_t Nak = 6;
constexpr size_t MaximumNameServers = 3;

struct Reply
{
    uint8_t type;
    uint8_t your_address[4];
    // The server identifier, which a request names to accept that server's offer.
    uint8_t server[4];
    bool has_netmask;
    uint8_t netmask[4];
    bool has_router;
    uint8_t router[4];
    size_t name_server_count;
    uint8_t name_servers[MaximumNameServers][4];
    // Each 0 when the server gave none.
    uint32_t lease_seconds;
    uint32_t renewal_seconds;
    uint32_t rebinding_seconds;
};

// A discover, or with requested and server, a request accepting that offer. Asks the server to
// broadcast its answers, since the client cannot take unicast before it has an address. Returns
// the message's length.
size_t BuildMessage(uint8_t (&message)[MessageSize], uint8_t type, uint32_t transaction,
                    const uint8_t (&mac)[6], const uint8_t* requested, const uint8_t* server);
// A request to extend the lease on client_address, which names only that address, so any server
// that knows the lease may answer, and takes the answer by unicast.
size_t BuildRenewal(uint8_t (&message)[MessageSize], uint32_t transaction, const uint8_t (&mac)[6],
                    const uint8_t (&client_address)[4]);
// A server's reply to transaction for mac, with a message type and well-formed options.
bool ParseReply(const uint8_t* message, size_t length, uint32_t transaction,
                const uint8_t (&mac)[6], Reply* reply);
// The netmask's prefix length, for one without gaps; -1 otherwise.
int NetmaskPrefixLength(const uint8_t (&netmask)[4]);

} // namespace FeatherOS::User::Dhcp

#endif
