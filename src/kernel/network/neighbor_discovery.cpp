#include "neighbor_discovery.h"

#include "icmpv6.h"
#include "interface.h"
#include "ip.h"
#include "name_servers.h"

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint8_t SlaacPrefixLength = 64;

struct AddressSlot
{
    // Version None until assigned.
    IpAddress address;
    uint64_t tentative_until;
    bool duplicate;
};

// Guards everything below.
constinit Sync::SpinLock configuration_lock;
constinit Scheduler::WaitQueue advertisement_waiters;
AddressSlot link_local = {};
AddressSlot global = {};
IpAddress router = {};
uint64_t router_until = 0;
bool advertised = false;

bool Usable(const AddressSlot& slot)
{
    return slot.address.version == IpVersion::V6 && !slot.duplicate;
}

AddressSlot* OwnSlotLocked(const IpAddress& address)
{
    if (Usable(link_local) && link_local.address == address)
    {
        return &link_local;
    }
    if (Usable(global) && global.address == address)
    {
        return &global;
    }
    return nullptr;
}

void AssignLocked(AddressSlot* slot, const IpAddress& address)
{
    slot->address = address;
    slot->duplicate = false;
    // One probe, then RetransTimer's default of a second without an answer (RFC 4861 10).
    slot->tentative_until = Time::TickCount() + Time::TicksPerSecond();
}

void SendNeighborMessage(const IpAddress& source, const IpAddress& destination,
                         const MacAddress& destination_mac, uint8_t* message, size_t length)
{
    SendIcmpv6(destination_mac, source, destination, NeighborDiscoveryHopLimit, message, length);
}

// From the unspecified address and without our link address, so that a host already using the
// address answers to all nodes without learning ours (RFC 4862 5.4.2).
void ProbeAddress(const IpAddress& address)
{
    uint8_t message[MaximumNeighborMessageSize];
    const size_t length = BuildNeighborSolicitation(message, address, nullptr);
    const IpAddress group = SolicitedNodeAddress(address);
    SendNeighborMessage(Ipv6Unspecified, group, MulticastMac(group), message, length);
}

void HandleRouterAdvertisement(const Ipv6Packet& packet)
{
    RouterAdvertisement advertisement;
    // RFC 4861 6.1.2: routers advertise from their link-local address.
    if (!IsLinkLocal(packet.source) ||
        !ParseRouterAdvertisement(packet.payload, packet.payload_length, &advertisement))
    {
        return;
    }
    if (advertisement.has_link_address)
    {
        LearnNeighbor(packet.source, advertisement.link_address, true);
    }
    if (advertisement.has_name_server)
    {
        const uint64_t lifetime = advertisement.name_server_lifetime;
        // All ones means forever (RFC 8106 5.1).
        AdvertiseNameServer(advertisement.name_server,
                            lifetime == 0 ? 0
                            : lifetime == 0xFFFFFFFF
                                ? UINT64_MAX
                                : Time::TickCount() + lifetime * Time::TicksPerSecond());
    }
    IpAddress address = {};
    bool probe = false;
    {
        Sync::SpinLockGuard guard(configuration_lock);
        if (advertisement.router_lifetime != 0)
        {
            router = packet.source;
            router_until =
                Time::TickCount() + advertisement.router_lifetime * Time::TicksPerSecond();
        }
        else if (router == packet.source)
        {
            router = {};
        }
        // Autoconfiguration needs the interface identifier's 64 bits after the prefix.
        if (advertisement.has_prefix && advertisement.prefix_length == SlaacPrefixLength &&
            advertisement.valid_lifetime != 0 && !IsLinkLocal(advertisement.prefix))
        {
            address = PrefixAddress(advertisement.prefix, LocalMac());
            if (!(global.address == address))
            {
                AssignLocked(&global, address);
                probe = true;
            }
        }
        advertised = true;
        Scheduler::WakeAll(advertisement_waiters);
    }
    if (probe)
    {
        ProbeAddress(address);
    }
}

void HandleAdvertisement(const MacAddress& source_mac, const NeighborMessage& message)
{
    {
        Sync::SpinLockGuard guard(configuration_lock);
        AddressSlot* slot = OwnSlotLocked(message.target);
        if (slot != nullptr)
        {
            // Another host answered for an address we are still probing.
            if (Time::TickCount() < slot->tentative_until && !(source_mac == LocalMac()))
            {
                slot->duplicate = true;
            }
            return;
        }
    }
    if (message.has_link_address)
    {
        LearnNeighbor(message.target, message.link_address, message.solicited);
    }
}

void HandleSolicitation(const MacAddress& source_mac, const Ipv6Packet& packet,
                        const NeighborMessage& message)
{
    const bool probe = IsUnspecified(packet.source);
    {
        Sync::SpinLockGuard guard(configuration_lock);
        AddressSlot* slot = OwnSlotLocked(message.target);
        if (slot == nullptr)
        {
            return;
        }
        // A tentative address gets no answers; a probe for it means another host wants it too
        // (RFC 4862 5.4.3), unless it is our own probe coming back.
        if (Time::TickCount() < slot->tentative_until)
        {
            if (probe && !(source_mac == LocalMac()))
            {
                slot->duplicate = true;
            }
            return;
        }
    }
    uint8_t reply[MaximumNeighborMessageSize];
    if (probe)
    {
        const size_t length = BuildNeighborAdvertisement(reply, message.target, LocalMac(), false);
        SendNeighborMessage(message.target, AllNodes, MulticastMac(AllNodes), reply, length);
        return;
    }
    if (message.has_link_address)
    {
        LearnNeighbor(packet.source, message.link_address, true);
    }
    const size_t length = BuildNeighborAdvertisement(reply, message.target, LocalMac(), true);
    SendNeighborMessage(message.target, packet.source,
                        message.has_link_address ? message.link_address : source_mac, reply,
                        length);
}

} // namespace

void StartIpv6()
{
    const IpAddress address = LinkLocalAddress(LocalMac());
    {
        Sync::SpinLockGuard guard(configuration_lock);
        AssignLocked(&link_local, address);
    }
    ProbeAddress(address);
    // RFC 4429 3.3: a solicitation from an optimistic address must leave out our link address.
    uint8_t message[MaximumNeighborMessageSize];
    const size_t length = BuildRouterSolicitation(message, nullptr);
    SendNeighborMessage(address, AllRouters, MulticastMac(AllRouters), message, length);
}

void WaitForRouter()
{
    const uint64_t deadline = Time::TickCount() + Time::TicksPerSecond();
    Sync::SpinLockGuard guard(configuration_lock);
    while (!advertised && Time::TickCount() < deadline)
    {
        Scheduler::WaitOn(advertisement_waiters, configuration_lock, deadline);
    }
}

void ForgetIpv6()
{
    {
        Sync::SpinLockGuard guard(configuration_lock);
        link_local = {};
        global = {};
        router = {};
        router_until = 0;
        advertised = false;
    }
    AdvertiseNameServer(Ipv6Unspecified, 0);
}

void ReadIpv6Configuration(Ipv6Configuration* configuration)
{
    Sync::SpinLockGuard guard(configuration_lock);
    configuration->link_local = Usable(link_local) ? link_local.address : Ipv6Unspecified;
    configuration->global = Usable(global) ? global.address : Ipv6Unspecified;
    configuration->prefix_length = SlaacPrefixLength;
    configuration->router = router.version == IpVersion::V6 && Time::TickCount() < router_until
                                ? router
                                : Ipv6Unspecified;
}

bool IsOwnIpv6Address(const IpAddress& address)
{
    Sync::SpinLockGuard guard(configuration_lock);
    return OwnSlotLocked(address) != nullptr;
}

bool IsJoinedIpv6Group(const IpAddress& address)
{
    if (address == AllNodes)
    {
        return true;
    }
    Sync::SpinLockGuard guard(configuration_lock);
    return (link_local.address.version == IpVersion::V6 &&
            address == SolicitedNodeAddress(link_local.address)) ||
           (global.address.version == IpVersion::V6 &&
            address == SolicitedNodeAddress(global.address));
}

IpAddress SelectIpv6Source(const IpAddress& destination)
{
    if (destination == Ipv6Loopback)
    {
        return destination;
    }
    Sync::SpinLockGuard guard(configuration_lock);
    if (OwnSlotLocked(destination) != nullptr)
    {
        return destination;
    }
    const AddressSlot& slot =
        IsLinkLocal(destination) || IsMulticast(destination) ? link_local : global;
    return Usable(slot) ? slot.address : Ipv6Unspecified;
}

bool Ipv6NextHop(const IpAddress& destination, IpAddress* next_hop)
{
    Sync::SpinLockGuard guard(configuration_lock);
    if (IsLinkLocal(destination) || (Usable(global) && SamePrefix64(destination, global.address)))
    {
        *next_hop = destination;
        return true;
    }
    if (router.version == IpVersion::V6 && Time::TickCount() < router_until)
    {
        *next_hop = router;
        return true;
    }
    return false;
}

void SolicitNeighbor(const IpAddress& target)
{
    const IpAddress source = SelectIpv6Source(target);
    if (IsUnspecified(source))
    {
        return;
    }
    uint8_t message[MaximumNeighborMessageSize];
    const MacAddress local = LocalMac();
    const size_t length = BuildNeighborSolicitation(message, target, &local);
    const IpAddress group = SolicitedNodeAddress(target);
    SendNeighborMessage(source, group, MulticastMac(group), message, length);
}

void HandleNeighborDiscovery(const MacAddress& source_mac, const Ipv6Packet& packet)
{
    // Anything that crossed a router has a lower hop limit (RFC 4861 6.1, 7.1).
    if (packet.hop_limit != NeighborDiscoveryHopLimit || packet.payload_length == 0)
    {
        return;
    }
    if (packet.payload[0] == RouterAdvertisementType)
    {
        HandleRouterAdvertisement(packet);
        return;
    }
    NeighborMessage message;
    if (!ParseNeighborMessage(packet.payload, packet.payload_length, &message) ||
        IsMulticast(message.target))
    {
        return;
    }
    if (message.type == NeighborAdvertisement)
    {
        HandleAdvertisement(source_mac, message);
    }
    else
    {
        HandleSolicitation(source_mac, packet, message);
    }
}

} // namespace FeatherOS::Network
