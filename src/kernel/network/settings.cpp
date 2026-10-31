#include "settings.h"

#include "interface.h"
#include "name_servers.h"
#include "neighbor_discovery.h"
#include "socket.h"

#include "kernel/console.h"
#include "kernel/string/format.h"

namespace FeatherOS::Network
{
namespace
{

void CopyAddress(uint8_t (&destination)[16], const IpAddress& address)
{
    for (size_t index = 0; index < sizeof(destination); ++index)
    {
        destination[index] = address.version == IpVersion::V6 ? address.bytes[index] : 0;
    }
}

void WriteIpv4(uint8_t (&destination)[4], Ipv4Address address)
{
    WriteBig32(destination, address);
}

void LogIpv4(const InterfaceConfiguration& configuration)
{
    char address[FormattedIpv4Length];
    char gateway[FormattedIpv4Length];
    FormatIpv4(configuration.address, address);
    FormatIpv4(configuration.gateway, gateway);
    char message[80];
    const bool formatted =
        configuration.address == 0
            ? String::FormatTo(message, sizeof(message), "KERN/I: Network: IPv4 unconfigured")
            : String::FormatTo(message, sizeof(message), "KERN/I: Network: IPv4 {}/{} via {}",
                               static_cast<const char*>(address),
                               PrefixLength(configuration.netmask),
                               static_cast<const char*>(gateway));
    if (formatted)
    {
        Console::WriteLine(message);
    }
}

} // namespace

uint64_t ReadSettings(User::NetworkSettings* settings)
{
    if (!IsUp())
    {
        return User::SyscallErrorNetworkUnreachable;
    }
    const MacAddress mac = LocalMac();
    for (size_t index = 0; index < sizeof(settings->mac); ++index)
    {
        settings->mac[index] = mac.bytes[index];
    }
    const InterfaceConfiguration ipv4 = Configuration();
    WriteIpv4(settings->ipv4_address, ipv4.address);
    WriteIpv4(settings->ipv4_gateway, ipv4.gateway);
    settings->ipv4_prefix_length = static_cast<uint8_t>(PrefixLength(ipv4.netmask));

    Ipv6Configuration ipv6;
    ReadIpv6Configuration(&ipv6);
    CopyAddress(settings->ipv6_link_local, ipv6.link_local);
    CopyAddress(settings->ipv6_global, ipv6.global);
    CopyAddress(settings->ipv6_router, ipv6.router);
    settings->ipv6_prefix_length = ipv6.prefix_length;
    settings->link_up = LinkIsUp() ? 1 : 0;

    NameServer servers[MaximumNameServers];
    const size_t count = ReadNameServers(servers);
    settings->name_server_count = static_cast<uint32_t>(count);
    for (size_t index = 0; index < User::MaximumNameServers; ++index)
    {
        settings->name_servers[index] =
            index < count ? Sockets::ToUserAddress(
                                {.address = servers[index].address, .port = servers[index].port})
                          : User::SocketAddress{};
    }
    return 0;
}

uint64_t ApplySettings(const User::NetworkSettings& settings, uint64_t changes)
{
    if (!IsUp())
    {
        return User::SyscallErrorNetworkUnreachable;
    }
    constexpr uint64_t KnownChanges =
        User::NetworkChangeIpv4 | User::NetworkChangeNameServers | User::NetworkChangeLink;
    if ((changes & ~KnownChanges) != 0 ||
        ((changes & User::NetworkChangeIpv4) != 0 && settings.ipv4_prefix_length > 32) ||
        ((changes & User::NetworkChangeNameServers) != 0 &&
         settings.name_server_count > User::MaximumNameServers) ||
        ((changes & User::NetworkChangeLink) != 0 && settings.link_up > 1))
    {
        return User::SyscallErrorInvalidArgument;
    }
    if ((changes & User::NetworkChangeLink) != 0 && !SetLink(settings.link_up != 0))
    {
        return User::SyscallErrorFailed;
    }
    if ((changes & User::NetworkChangeIpv4) != 0 && !LinkIsUp())
    {
        return User::SyscallErrorNetworkDown;
    }
    NameServer servers[MaximumNameServers];
    const size_t count = settings.name_server_count;
    if ((changes & User::NetworkChangeNameServers) != 0)
    {
        for (size_t index = 0; index < count; ++index)
        {
            const Sockets::Endpoint server = Sockets::FromUserAddress(settings.name_servers[index]);
            if (server.address.version == IpVersion::None)
            {
                return User::SyscallErrorFamilyNotSupported;
            }
            servers[index].address = server.address;
            servers[index].port = server.port == 0 ? DnsPort : server.port;
        }
        SetNameServers(servers, count);
    }
    if ((changes & User::NetworkChangeIpv4) != 0)
    {
        const InterfaceConfiguration configuration = {
            .address = ReadBig32(settings.ipv4_address),
            .netmask = PrefixNetmask(settings.ipv4_prefix_length),
            .gateway = ReadBig32(settings.ipv4_gateway),
        };
        if (!SetConfiguration(configuration))
        {
            return User::SyscallErrorNetworkDown;
        }
        LogIpv4(configuration);
    }
    return 0;
}

} // namespace FeatherOS::Network
