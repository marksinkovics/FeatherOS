#include "ip.h"

#include "checksum.h"
#include "icmp.h"
#include "icmpv6.h"
#include "interface.h"
#include "ipv4.h"
#include "ipv6.h"
#include "neighbor_discovery.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"

#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint8_t DefaultHopLimit = 64;

uint16_t next_identification = 1;
constinit Sync::SpinLock icmp_error_lock;
IcmpErrorLimit icmp_error_limit = {.tokens = IcmpErrorBurst, .refilled_at_ms = 0};

void FillChecksum(const IpAddress& source, const IpAddress& destination, uint8_t protocol,
                  uint8_t* payload, size_t length, size_t offset)
{
    if (offset == NoChecksum || offset + 2 > length)
    {
        return;
    }
    payload[offset] = 0;
    payload[offset + 1] = 0;
    const uint64_t sum = PseudoHeaderSum(source, destination, protocol, length);
    const uint16_t checksum = ChecksumFinish(ChecksumAdd(sum, payload, length));
    // UDP reserves 0 for "no checksum"; 0xFFFF is the same value in ones' complement.
    WriteBig16(payload + offset, checksum == 0 ? 0xFFFF : checksum);
}

void CopyBytes(uint8_t* destination, const uint8_t* source, size_t length)
{
    for (size_t index = 0; index < length; ++index)
    {
        destination[index] = source[index];
    }
}

Ipv4Address NextHop(Ipv4Address destination)
{
    const InterfaceConfiguration configuration = Configuration();
    if (destination == Ipv4Broadcast ||
        (destination & configuration.netmask) == (configuration.address & configuration.netmask))
    {
        return destination;
    }
    return configuration.gateway;
}

bool TransmitIpv6(const MacAddress& destination_mac, const Ipv6Packet& fields)
{
    uint8_t packet[MaximumIpv6PacketSize];
    const size_t length = BuildIpv6Packet(packet, fields);
    return length != 0 && SendFrame(destination_mac, EtherTypeIpv6, packet, length);
}

// A reply goes back to the hardware address the request came from, so answering never waits on
// address resolution; a loopback request has none and loops back again.
void ReplyIpv4(const Ipv4Packet& request, const MacAddress* source_mac, uint8_t* reply,
               size_t length)
{
    if (source_mac == nullptr)
    {
        SendIp(MakeIpAddress(request.source), IpProtocolIcmp, reply, length);
        return;
    }
    const Ipv4Packet fields = {
        .source = request.destination,
        .destination = request.source,
        .protocol = IpProtocolIcmp,
        .payload = reply,
        .payload_length = length,
    };
    uint8_t packet[MaximumIpv4PacketSize];
    const size_t packet_length = BuildIpv4Packet(
        packet, fields, __atomic_fetch_add(&next_identification, 1, __ATOMIC_RELAXED));
    if (packet_length != 0)
    {
        SendFrame(*source_mac, EtherTypeIpv4, packet, packet_length);
    }
}

void ReplyIpv6(const Ipv6Packet& request, const MacAddress* source_mac, uint8_t* reply,
               size_t length)
{
    if (source_mac == nullptr)
    {
        SendIp(request.source, IpProtocolIcmpv6, reply, length, Icmpv6ChecksumOffset);
        return;
    }
    SendIcmpv6(*source_mac, request.destination, request.source, DefaultHopLimit, reply, length);
}

void AnswerIpv4Echo(const Ipv4Packet& request, const MacAddress* source_mac)
{
    uint8_t reply[MaximumIpv4Payload];
    CopyBytes(reply, request.payload, request.payload_length);
    RewriteIcmpEcho(reply, request.payload_length, IcmpEchoReply, ReadBig16(reply + 4));
    ReplyIpv4(request, source_mac, reply, request.payload_length);
}

void AnswerIpv6Echo(const Ipv6Packet& request, const MacAddress* source_mac)
{
    uint8_t reply[MaximumIpv6Payload];
    CopyBytes(reply, request.payload, request.payload_length);
    reply[0] = Icmpv6EchoReply;
    ReplyIpv6(request, source_mac, reply, request.payload_length);
}

bool TakeIcmpError()
{
    Sync::SpinLockGuard guard(icmp_error_lock);
    return TakeIcmpErrorToken(&icmp_error_limit, Time::TickCount() * 1000 / Time::TicksPerSecond());
}

// bytes is the whole packet, which the error quotes.
void SendIpv4PortUnreachable(const Ipv4Packet& packet, const uint8_t* bytes,
                             const MacAddress* source_mac)
{
    if (!TakeIcmpError())
    {
        return;
    }
    uint8_t message[MaximumIcmpErrorSize];
    const size_t length = BuildIcmpPortUnreachable(message, bytes, ReadBig16(bytes + 2));
    ReplyIpv4(packet, source_mac, message, length);
}

void SendIpv6PortUnreachable(const Ipv6Packet& packet, const uint8_t* bytes,
                             const MacAddress* source_mac)
{
    if (!TakeIcmpError())
    {
        return;
    }
    uint8_t message[MaximumIcmpv6ErrorSize];
    const size_t length =
        BuildIcmpv6PortUnreachable(message, bytes, Ipv6HeaderSize + ReadBig16(bytes + 4));
    ReplyIpv6(packet, source_mac, message, length);
}

void DeliverUnreachable(const IcmpUnreachable& error)
{
    if (error.protocol == IpProtocolUdp)
    {
        Sockets::DeliverPortUnreachable(
            {.address = error.source, .port = error.source_port},
            {.address = error.destination, .port = error.destination_port});
    }
}

void HandleIcmp(const Ipv4Packet& packet, const MacAddress* source_mac, bool to_us)
{
    IcmpUnreachable error;
    if (to_us && ParseIcmpUnreachable(packet.payload, packet.payload_length, &error))
    {
        if (error.code == IcmpPortUnreachable)
        {
            DeliverUnreachable(error);
        }
        return;
    }
    IcmpEcho echo;
    if (!ParseIcmpEcho(packet.payload, packet.payload_length, &echo))
    {
        return;
    }
    // Like Linux by default, only pings to our own address get an answer, not broadcast ones.
    if (echo.type == IcmpEchoRequest && to_us)
    {
        AnswerIpv4Echo(packet, source_mac);
    }
    else if (echo.type == IcmpEchoReply)
    {
        Sockets::DeliverEcho(MakeIpAddress(packet.source), echo.identifier, packet.payload,
                             packet.payload_length);
    }
}

// Returns false for a valid datagram that no socket takes.
bool HandleUdp(const IpAddress& source, const IpAddress& destination, const uint8_t* payload,
               size_t length)
{
    UdpDatagram datagram;
    if (!ParseUdpDatagram(payload, length, &datagram))
    {
        return true;
    }
    const size_t udp_length = UdpHeaderSize + datagram.payload_length;
    // Only IPv4 lets a sender leave the checksum out (RFC 8200 8.1).
    if (datagram.checksum == 0
            ? source.version == IpVersion::V6
            : ChecksumFinish(
                  ChecksumAdd(PseudoHeaderSum(source, destination, IpProtocolUdp, udp_length),
                              payload, udp_length)) != 0)
    {
        return true;
    }
    return Sockets::DeliverUdp({.address = source, .port = datagram.source_port},
                               {.address = destination, .port = datagram.destination_port},
                               datagram.payload, datagram.payload_length);
}

void HandleTcp(const IpAddress& source, const IpAddress& destination, const uint8_t* payload,
               size_t length)
{
    TcpSegment segment;
    if (ChecksumFinish(ChecksumAdd(PseudoHeaderSum(source, destination, IpProtocolTcp, length),
                                   payload, length)) != 0 ||
        !ParseTcpSegment(payload, length, &segment))
    {
        return;
    }
    Sockets::DeliverTcp({.address = source, .port = segment.source_port},
                        {.address = destination, .port = segment.destination_port}, segment);
}

// Errors are only for datagrams from one host to us (RFC 1122 3.2.2), never for broadcasts.
bool IsUnicastIpv4Source(Ipv4Address source, const MacAddress* source_mac)
{
    const InterfaceConfiguration configuration = Configuration();
    return source != 0 && source != Ipv4Broadcast && (source >> 28) != 0xE &&
           (configuration.netmask == 0xFFFFFFFF ||
            source != (configuration.address | ~configuration.netmask)) &&
           (source_mac == nullptr || !IsIpv4Loopback(source));
}

// bytes is the whole packet, header included.
void HandleIpv4(const Ipv4Packet& packet, const MacAddress* source_mac, const uint8_t* bytes)
{
    const InterfaceConfiguration configuration = Configuration();
    const bool configured = configuration.address != 0;
    const Ipv4Address subnet_broadcast = configuration.address | ~configuration.netmask;
    const bool to_us = (configured && packet.destination == configuration.address) ||
                       (source_mac == nullptr && IsIpv4Loopback(packet.destination));
    // Before it has an address, a DHCP client takes what a server sends to the address it offers
    // (RFC 2131 4.1).
    if (!to_us && packet.destination != Ipv4Broadcast && packet.destination != subnet_broadcast &&
        (configured || source_mac == nullptr))
    {
        return;
    }
    if (packet.protocol == IpProtocolIcmp)
    {
        HandleIcmp(packet, source_mac, to_us);
    }
    else if (packet.protocol == IpProtocolUdp)
    {
        if (!HandleUdp(MakeIpAddress(packet.source), MakeIpAddress(packet.destination),
                       packet.payload, packet.payload_length) &&
            to_us && IsUnicastIpv4Source(packet.source, source_mac))
        {
            SendIpv4PortUnreachable(packet, bytes, source_mac);
        }
    }
    // Connections never involve a broadcast address.
    else if (packet.protocol == IpProtocolTcp && to_us)
    {
        HandleTcp(MakeIpAddress(packet.source), MakeIpAddress(packet.destination), packet.payload,
                  packet.payload_length);
    }
}

void HandleIcmpv6(const Ipv6Packet& packet, const MacAddress* source_mac, bool to_us)
{
    // Unlike ICMP for IPv4, the checksum covers the pseudo-header too.
    const uint64_t sum =
        PseudoHeaderSum(packet.source, packet.destination, IpProtocolIcmpv6, packet.payload_length);
    if (packet.payload_length < IcmpHeaderSize ||
        ChecksumFinish(ChecksumAdd(sum, packet.payload, packet.payload_length)) != 0)
    {
        return;
    }
    const uint8_t type = packet.payload[0];
    IcmpEcho echo;
    IcmpUnreachable error;
    if (type == Icmpv6DestinationUnreachable)
    {
        if (to_us && ParseIcmpv6Unreachable(packet.payload, packet.payload_length, &error) &&
            error.code == Icmpv6PortUnreachable)
        {
            DeliverUnreachable(error);
        }
    }
    else if (type == Icmpv6EchoRequest || type == Icmpv6EchoReply)
    {
        if (!ParseIcmpv6Echo(packet.payload, packet.payload_length, &echo))
        {
            return;
        }
        if (type == Icmpv6EchoRequest && to_us)
        {
            AnswerIpv6Echo(packet, source_mac);
        }
        else if (type == Icmpv6EchoReply)
        {
            Sockets::DeliverEcho(packet.source, echo.identifier, packet.payload,
                                 packet.payload_length);
        }
    }
    else if (type >= RouterSolicitation && type <= NeighborAdvertisement && source_mac != nullptr)
    {
        HandleNeighborDiscovery(*source_mac, packet);
    }
}

// bytes is the whole packet, header included.
void HandleIpv6(const Ipv6Packet& packet, const MacAddress* source_mac, const uint8_t* bytes)
{
    const bool loopback = source_mac == nullptr;
    // ::1 never appears on the wire (RFC 4291 2.5.3).
    if (IsMulticast(packet.source) ||
        (!loopback && (packet.source == Ipv6Loopback || packet.destination == Ipv6Loopback)))
    {
        return;
    }
    const bool to_us = packet.destination == Ipv6Loopback || IsOwnIpv6Address(packet.destination);
    if ((to_us || IsJoinedIpv6Group(packet.destination)) && packet.next_header == IpProtocolIcmpv6)
    {
        HandleIcmpv6(packet, source_mac, to_us);
    }
    else if (to_us && packet.next_header == IpProtocolUdp)
    {
        if (!HandleUdp(packet.source, packet.destination, packet.payload, packet.payload_length) &&
            !IsUnspecified(packet.source))
        {
            SendIpv6PortUnreachable(packet, bytes, source_mac);
        }
    }
    else if (to_us && packet.next_header == IpProtocolTcp)
    {
        HandleTcp(packet.source, packet.destination, packet.payload, packet.payload_length);
    }
}

SendStatus SendIpv4(const IpAddress& destination, uint8_t protocol, uint8_t* payload, size_t length,
                    size_t checksum_offset)
{
    if (length > MaximumIpv4Payload)
    {
        return SendStatus::TooLong;
    }
    const Ipv4Address target = ToIpv4(destination);
    const Ipv4Packet fields = {
        .source = ToIpv4(SourceAddressFor(destination)),
        .destination = target,
        .protocol = protocol,
        .payload = payload,
        .payload_length = length,
    };
    FillChecksum(MakeIpAddress(fields.source), destination, protocol, payload, length,
                 checksum_offset);
    uint8_t packet[MaximumIpv4PacketSize];
    const size_t packet_length = BuildIpv4Packet(
        packet, fields, __atomic_fetch_add(&next_identification, 1, __ATOMIC_RELAXED));
    if (IsLocalAddress(destination))
    {
        HandleIpv4(fields, nullptr, packet);
        return SendStatus::Sent;
    }

    MacAddress next_hop = BroadcastMac;
    if (target != Ipv4Broadcast && !Resolve(MakeIpAddress(NextHop(target)), &next_hop))
    {
        return SendStatus::HostUnreachable;
    }
    return SendFrame(next_hop, EtherTypeIpv4, packet, packet_length) ? SendStatus::Sent
                                                                     : SendStatus::NoBuffers;
}

SendStatus SendIpv6(const IpAddress& destination, uint8_t protocol, uint8_t* payload, size_t length,
                    size_t checksum_offset)
{
    if (length > MaximumIpv6Payload)
    {
        return SendStatus::TooLong;
    }
    const IpAddress source = SourceAddressFor(destination);
    if (IsUnspecified(destination) || IsUnspecified(source))
    {
        return SendStatus::NoRoute;
    }
    FillChecksum(source, destination, protocol, payload, length, checksum_offset);
    const Ipv6Packet fields = {.source = source,
                               .destination = destination,
                               .next_header = protocol,
                               .hop_limit = DefaultHopLimit,
                               .payload = payload,
                               .payload_length = length};
    if (IsLocalAddress(destination))
    {
        uint8_t packet[MaximumIpv6PacketSize];
        BuildIpv6Packet(packet, fields);
        HandleIpv6(fields, nullptr, packet);
        return SendStatus::Sent;
    }

    MacAddress next_hop_mac;
    if (IsMulticast(destination))
    {
        next_hop_mac = MulticastMac(destination);
    }
    else
    {
        IpAddress next_hop;
        if (!Ipv6NextHop(destination, &next_hop))
        {
            return SendStatus::NoRoute;
        }
        if (!Resolve(next_hop, &next_hop_mac))
        {
            return SendStatus::HostUnreachable;
        }
    }
    return TransmitIpv6(next_hop_mac, fields) ? SendStatus::Sent : SendStatus::NoBuffers;
}

} // namespace

bool IsLocalAddress(const IpAddress& address)
{
    if (!IsUp())
    {
        return false;
    }
    if (address.version == IpVersion::V6)
    {
        return address == Ipv6Loopback || IsOwnIpv6Address(address);
    }
    const Ipv4Address ipv4 = ToIpv4(address);
    return address.version == IpVersion::V4 &&
           (ipv4 == Configuration().address || IsIpv4Loopback(ipv4));
}

IpAddress SourceAddressFor(const IpAddress& destination)
{
    if (destination.version == IpVersion::V6)
    {
        return SelectIpv6Source(destination);
    }
    const Ipv4Address target = ToIpv4(destination);
    return MakeIpAddress(IsIpv4Loopback(target) ? target : Configuration().address);
}

SendStatus SendIp(const IpAddress& destination, uint8_t protocol, uint8_t* payload, size_t length,
                  size_t checksum_offset)
{
    if (!IsUp())
    {
        return SendStatus::NoRoute;
    }
    // Down, our own addresses are gone, so only loopback ones are local.
    if (!LinkIsUp() && !IsLocalAddress(destination))
    {
        return SendStatus::NetworkDown;
    }
    if (destination.version == IpVersion::V4)
    {
        return SendIpv4(destination, protocol, payload, length, checksum_offset);
    }
    if (destination.version == IpVersion::V6)
    {
        return SendIpv6(destination, protocol, payload, length, checksum_offset);
    }
    return SendStatus::NoRoute;
}

void ReceiveIpv4(const MacAddress& source, const uint8_t* packet, size_t length)
{
    Ipv4Packet parsed;
    if (ParseIpv4Packet(packet, length, &parsed))
    {
        HandleIpv4(parsed, &source, packet);
    }
}

void ReceiveIpv6(const MacAddress& source, const uint8_t* packet, size_t length)
{
    Ipv6Packet parsed;
    if (ParseIpv6Packet(packet, length, &parsed))
    {
        HandleIpv6(parsed, &source, packet);
    }
}

bool SendIcmpv6(const MacAddress& destination_mac, const IpAddress& source,
                const IpAddress& destination, uint8_t hop_limit, uint8_t* message, size_t length)
{
    if (length > MaximumIpv6Payload)
    {
        return false;
    }
    FillChecksum(source, destination, IpProtocolIcmpv6, message, length, Icmpv6ChecksumOffset);
    const Ipv6Packet fields = {.source = source,
                               .destination = destination,
                               .next_header = IpProtocolIcmpv6,
                               .hop_limit = hop_limit,
                               .payload = message,
                               .payload_length = length};
    return TransmitIpv6(destination_mac, fields);
}

} // namespace FeatherOS::Network
