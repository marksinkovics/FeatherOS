#include "neighbor_cache.h"

namespace FeatherOS::Network
{

void NeighborCache::Insert(const IpAddress& address, const MacAddress& mac, uint64_t expires_tick)
{
    Entry* slot = &entries[0];
    for (Entry& entry : entries)
    {
        if (entry.expires_tick != 0 && entry.address == address)
        {
            slot = &entry;
            break;
        }
        if (entry.expires_tick < slot->expires_tick)
        {
            slot = &entry;
        }
    }
    slot->address = address;
    slot->mac = mac;
    slot->expires_tick = expires_tick;
}

bool NeighborCache::Contains(const IpAddress& address, uint64_t now_tick) const
{
    MacAddress mac;
    return Lookup(address, now_tick, &mac);
}

bool NeighborCache::Lookup(const IpAddress& address, uint64_t now_tick, MacAddress* mac) const
{
    for (const Entry& entry : entries)
    {
        if (entry.expires_tick > now_tick && entry.address == address)
        {
            *mac = entry.mac;
            return true;
        }
    }
    return false;
}

void NeighborCache::Clear()
{
    for (Entry& entry : entries)
    {
        entry = {};
    }
}

} // namespace FeatherOS::Network
