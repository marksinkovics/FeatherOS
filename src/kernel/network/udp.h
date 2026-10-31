#ifndef FEATHEROS_NETWORK_UDP_H
#define FEATHEROS_NETWORK_UDP_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

constexpr size_t UdpHeaderSize = 8;
constexpr size_t UdpChecksumOffset = 6;
constexpr uint8_t IpProtocolUdp = 17;

struct UdpDatagram
{
    uint16_t source_port;
    uint16_t destination_port;
    // 0 when the sender computed none, which only IPv4 allows.
    uint16_t checksum;
    const uint8_t* payload;
    size_t payload_length;
};

// Checks the header's length against the data, which may carry padding past it; the checksum is
// left to the caller, which knows the pseudo-header. The payload points into data.
bool ParseUdpDatagram(const uint8_t* data, size_t length, UdpDatagram* datagram);
// Writes the header in front of a payload of payload_length bytes, with a zero checksum for the
// IP layer to fill in.
void WriteUdpHeader(uint8_t* data, uint16_t source_port, uint16_t destination_port,
                    size_t payload_length);

} // namespace FeatherOS::Network

#endif
