#ifndef FEATHEROS_NETWORK_NEIGHBOR_CACHE_H
#define FEATHEROS_NETWORK_NEIGHBOR_CACHE_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

// Hardware addresses of hosts on the local network, learned through ARP for IPv4 and neighbor
// discovery for IPv6.
class NeighborCache
{
public:
    static constexpr size_t Capacity = 16;

    constexpr NeighborCache() = default;
    // Adds or refreshes an entry; when full, the entry closest to expiring makes room.
    void Insert(const IpAddress& address, const MacAddress& mac, uint64_t expires_tick);
    bool Contains(const IpAddress& address, uint64_t now_tick) const;
    bool Lookup(const IpAddress& address, uint64_t now_tick, MacAddress* mac) const;
    void Clear();

private:
    struct Entry
    {
        IpAddress address;
        MacAddress mac;
        uint64_t expires_tick;
    };

    Entry entries[Capacity] = {};
};

} // namespace FeatherOS::Network

#endif
