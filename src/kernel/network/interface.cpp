#include "interface.h"

#include "arp.h"
#include "device.h"
#include "ethernet.h"
#include "ip.h"
#include "neighbor_cache.h"
#include "neighbor_discovery.h"
#include "socket.h"

#include "kernel/console.h"
#include "kernel/scheduler.h"
#include "kernel/sync/mutex.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint32_t ResolveAttempts = 3;
constexpr uint64_t NeighborLifetimeSeconds = 300;

// Unconfigured, 0.0.0.0, until DHCP or the user sets it.
constinit Sync::SpinLock configuration_lock;
InterfaceConfiguration configuration = {};
MacAddress local_mac = {};
bool up = false;
bool link_up = false;
// Held while a frame is handled and while the link changes, so no frame that arrived before the
// link went down is handled after its state was dropped.
constinit Sync::Mutex link_mutex;
uint8_t received_frame[MaximumFrameSize];
constinit NeighborCache neighbor_cache;
constinit Sync::SpinLock neighbor_lock;
constinit Scheduler::WaitQueue neighbor_waiters;

bool SendArp(uint16_t operation, const MacAddress& destination, const MacAddress& target_mac,
             Ipv4Address target_address)
{
    uint8_t packet[ArpPacketSize];
    WriteArpPacket(packet, {
                               .operation = operation,
                               .sender_mac = local_mac,
                               .sender_address = Configuration().address,
                               .target_mac = target_mac,
                               .target_address = target_address,
                           });
    return SendFrame(destination, EtherTypeArp, packet, sizeof(packet));
}

void HandleArp(const uint8_t* payload, size_t length)
{
    ArpPacket packet;
    if (!ParseArpPacket(payload, length, &packet) || packet.sender_address == 0)
    {
        return;
    }
    const Ipv4Address address = Configuration().address;
    const bool for_us = address != 0 && packet.target_address == address;
    // A reply to our own request, which comes back to 0.0.0.0 before we have an address.
    const bool answer = packet.operation == ArpReply && packet.target_mac == local_mac;
    LearnNeighbor(MakeIpAddress(packet.sender_address), packet.sender_mac, for_us || answer);
    if (for_us && packet.operation == ArpRequest)
    {
        SendArp(ArpReply, packet.sender_mac, packet.sender_mac, packet.sender_address);
    }
}

void HandleFrame(const uint8_t* frame, size_t length)
{
    EthernetFrame parsed;
    if (!ParseEthernetFrame(frame, length, &parsed))
    {
        return;
    }
    // IPv6 multicast goes to 33:33 and the group's last 32 bits (RFC 2464).
    const bool multicast =
        parsed.destination.bytes[0] == 0x33 && parsed.destination.bytes[1] == 0x33;
    if (!(parsed.destination == local_mac || parsed.destination == BroadcastMac || multicast))
    {
        return;
    }
    if (parsed.type == EtherTypeArp)
    {
        HandleArp(parsed.payload, parsed.payload_length);
    }
    else if (parsed.type == EtherTypeIpv4)
    {
        ReceiveIpv4(parsed.source, parsed.payload, parsed.payload_length);
    }
    else if (parsed.type == EtherTypeIpv6)
    {
        ReceiveIpv6(parsed.source, parsed.payload, parsed.payload_length);
    }
}

uint64_t RunReceiveTask(void*)
{
    for (;;)
    {
        const size_t length = Device::Receive(received_frame);
        Sync::MutexGuard guard(link_mutex);
        if (LinkIsUp())
        {
            HandleFrame(received_frame, length);
        }
    }
}

} // namespace

bool Initialize(const void* platform_data, uint64_t hhdm_offset)
{
    if (up || !Device::Initialize(platform_data, hhdm_offset))
    {
        return false;
    }
    local_mac = Device::Address();
    Scheduler::TaskId task;
    if (!Scheduler::CreateTask(RunReceiveTask, nullptr, &task) ||
        !Scheduler::SetPriority(task, Scheduler::TaskPriority::High) || !Sockets::StartTcp())
    {
        return false;
    }
    up = true;
    __atomic_store_n(&link_up, true, __ATOMIC_RELEASE);
    return true;
}

bool IsUp()
{
    return up;
}

bool LinkIsUp()
{
    return __atomic_load_n(&link_up, __ATOMIC_ACQUIRE);
}

bool SetLink(bool new_state)
{
    if (!up)
    {
        return false;
    }
    {
        Sync::MutexGuard guard(link_mutex);
        if (LinkIsUp() == new_state)
        {
            return true;
        }
        if (!new_state)
        {
            __atomic_store_n(&link_up, false, __ATOMIC_RELEASE);
            Console::WriteLine("KERN/I: Network: link down");
            Device::Stop();
            {
                // Under the lock SetConfiguration checks the link with, so no address set
                // while the link went down outlasts this.
                Sync::SpinLockGuard configuration_guard(configuration_lock);
                configuration = {};
            }
            ForgetIpv6();
            {
                Sync::SpinLockGuard neighbor_guard(neighbor_lock);
                neighbor_cache.Clear();
                Scheduler::WakeAll(neighbor_waiters);
            }
            Sockets::DropExternalConnections();
            return true;
        }
        if (!Device::Restart())
        {
            return false;
        }
        __atomic_store_n(&link_up, true, __ATOMIC_RELEASE);
        StartIpv6();
    }
    // The receive task handles the router's advertisement, which needs the mutex.
    WaitForRouter();
    Console::WriteLine("KERN/I: Network: link up");
    return true;
}

MacAddress LocalMac()
{
    return local_mac;
}

bool HasRoute()
{
    if (!up || !LinkIsUp())
    {
        return false;
    }
    const InterfaceConfiguration ipv4 = Configuration();
    if (ipv4.address != 0 && ipv4.gateway != 0)
    {
        return true;
    }
    Ipv6Configuration ipv6;
    ReadIpv6Configuration(&ipv6);
    return !IsUnspecified(ipv6.global) && !IsUnspecified(ipv6.router);
}

InterfaceConfiguration Configuration()
{
    Sync::SpinLockGuard guard(configuration_lock);
    return configuration;
}

bool SetConfiguration(const InterfaceConfiguration& replacement)
{
    Sync::SpinLockGuard guard(configuration_lock);
    if (!LinkIsUp())
    {
        return false;
    }
    configuration = replacement;
    return true;
}

bool Resolve(const IpAddress& address, MacAddress* mac)
{
    if (!up || mac == nullptr || address.version == IpVersion::None)
    {
        return false;
    }
    for (uint32_t attempt = 0; attempt < ResolveAttempts && LinkIsUp(); ++attempt)
    {
        {
            Sync::SpinLockGuard guard(neighbor_lock);
            if (neighbor_cache.Lookup(address, Time::TickCount(), mac))
            {
                return true;
            }
        }
        if (address.version == IpVersion::V4)
        {
            constexpr MacAddress Unknown = {};
            SendArp(ArpRequest, BroadcastMac, Unknown, ToIpv4(address));
        }
        else
        {
            SolicitNeighbor(address);
        }

        const uint64_t deadline = Time::TickCount() + Time::TicksPerSecond();
        Sync::SpinLockGuard guard(neighbor_lock);
        while (Time::TickCount() < deadline && LinkIsUp())
        {
            if (neighbor_cache.Lookup(address, Time::TickCount(), mac))
            {
                return true;
            }
            Scheduler::WaitOn(neighbor_waiters, neighbor_lock, deadline);
        }
    }
    return false;
}

void LearnNeighbor(const IpAddress& address, const MacAddress& mac, bool learn_new)
{
    Sync::SpinLockGuard guard(neighbor_lock);
    const uint64_t now = Time::TickCount();
    if (learn_new || neighbor_cache.Contains(address, now))
    {
        neighbor_cache.Insert(address, mac, now + NeighborLifetimeSeconds * Time::TicksPerSecond());
        Scheduler::WakeAll(neighbor_waiters);
    }
}

bool SendFrame(const MacAddress& destination, uint16_t type, const uint8_t* payload, size_t length)
{
    if (!LinkIsUp())
    {
        return false;
    }
    uint8_t frame[MaximumFrameSize];
    const size_t frame_length =
        BuildEthernetFrame(frame, destination, local_mac, type, payload, length);
    return frame_length != 0 && Device::Transmit(frame, frame_length);
}

} // namespace FeatherOS::Network
