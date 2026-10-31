#include "name_servers.h"

#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Network
{
namespace
{

// Guards everything below.
constinit Sync::SpinLock name_server_lock;
NameServer configured[MaximumNameServers] = {};
size_t configured_count = 0;
IpAddress advertised = {};
uint64_t advertised_until = 0;

} // namespace

void SetNameServers(const NameServer* servers, size_t count)
{
    Sync::SpinLockGuard guard(name_server_lock);
    configured_count = count < MaximumNameServers ? count : MaximumNameServers;
    for (size_t index = 0; index < configured_count; ++index)
    {
        configured[index] = servers[index];
    }
}

void AdvertiseNameServer(const IpAddress& address, uint64_t until)
{
    Sync::SpinLockGuard guard(name_server_lock);
    advertised = address;
    advertised_until = until;
}

size_t ReadNameServers(NameServer (&servers)[MaximumNameServers])
{
    Sync::SpinLockGuard guard(name_server_lock);
    size_t count = 0;
    bool listed = false;
    for (; count < configured_count; ++count)
    {
        servers[count] = configured[count];
        listed = listed ||
                 (configured[count].address == advertised && configured[count].port == DnsPort);
    }
    if (!listed && count < MaximumNameServers && Time::TickCount() < advertised_until)
    {
        servers[count].address = advertised;
        servers[count].port = DnsPort;
        ++count;
    }
    return count;
}

} // namespace FeatherOS::Network
