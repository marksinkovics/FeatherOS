#include "support/network_stubs.h"

#include "kernel/network/interface.h"
#include "kernel/network/ipv4.h"
#include "kernel/network/ipv6.h"

namespace TestNetwork
{

std::vector<SentPacket> sent;
FeatherOS::Network::SendStatus status = FeatherOS::Network::SendStatus::Sent;
FeatherOS::Network::IpAddress local_address =
    FeatherOS::Network::MakeIpAddress(FeatherOS::Network::MakeIpv4(10, 0, 2, 15));
FeatherOS::Network::IpAddress local_ipv6_address = {
    .version = FeatherOS::Network::IpVersion::V6,
    .bytes = {0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0x50, 0x54, 0, 0xFF, 0xFE, 0x12, 0x34, 0x56}};
bool link_up = true;

void Reset()
{
    sent.clear();
    status = FeatherOS::Network::SendStatus::Sent;
    link_up = true;
}

} // namespace TestNetwork

namespace FeatherOS::Network
{

SendStatus SendIp(const IpAddress& destination, uint8_t protocol, uint8_t* payload, size_t length,
                  size_t checksum_offset)
{
    TestNetwork::sent.push_back(
        {destination, protocol, {payload, payload + length}, checksum_offset});
    return TestNetwork::status;
}

bool IsLocalAddress(const IpAddress& address)
{
    return address == TestNetwork::local_address;
}

IpAddress SourceAddressFor(const IpAddress& destination)
{
    if (destination == Ipv6Loopback ||
        (destination.version == IpVersion::V4 && IsIpv4Loopback(ToIpv4(destination))))
    {
        return destination;
    }
    if (!TestNetwork::link_up)
    {
        return destination.version == IpVersion::V6 ? Ipv6Unspecified : MakeIpAddress(0);
    }
    if (destination.version == IpVersion::V6)
    {
        return TestNetwork::local_ipv6_address;
    }
    return TestNetwork::local_address;
}

bool LinkIsUp()
{
    return TestNetwork::link_up;
}

} // namespace FeatherOS::Network
