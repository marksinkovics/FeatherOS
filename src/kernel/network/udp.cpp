#include "udp.h"

#include "address.h"

namespace FeatherOS::Network
{

bool ParseUdpDatagram(const uint8_t* data, size_t length, UdpDatagram* datagram)
{
    if (data == nullptr || datagram == nullptr || length < UdpHeaderSize)
    {
        return false;
    }
    const uint16_t total = ReadBig16(data + 4);
    if (total < UdpHeaderSize || total > length)
    {
        return false;
    }
    datagram->source_port = ReadBig16(data);
    datagram->destination_port = ReadBig16(data + 2);
    datagram->checksum = ReadBig16(data + UdpChecksumOffset);
    datagram->payload = data + UdpHeaderSize;
    datagram->payload_length = total - UdpHeaderSize;
    return true;
}

void WriteUdpHeader(uint8_t* data, uint16_t source_port, uint16_t destination_port,
                    size_t payload_length)
{
    WriteBig16(data, source_port);
    WriteBig16(data + 2, destination_port);
    WriteBig16(data + 4, static_cast<uint16_t>(UdpHeaderSize + payload_length));
    WriteBig16(data + UdpChecksumOffset, 0);
}

} // namespace FeatherOS::Network
