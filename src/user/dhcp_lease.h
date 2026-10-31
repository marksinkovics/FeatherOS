#ifndef FEATHEROS_USER_DHCP_LEASE_H
#define FEATHEROS_USER_DHCP_LEASE_H

#include <stdint.h>

// When a DHCP client renews, rebinds and gives up its lease (RFC 2131 4.4.5). Times are in
// milliseconds on the monotonic clock.
namespace FeatherOS::User::Dhcp
{

constexpr uint32_t InfiniteLease = 0xFFFFFFFF;
constexpr uint64_t Never = UINT64_MAX;

enum class LeasePhase
{
    Bound,
    // Asking the server that granted the lease.
    Renewing,
    // Asking any server, by broadcast.
    Rebinding,
    Expired,
};

struct LeaseTimes
{
    uint64_t renew_at;
    uint64_t rebind_at;
    uint64_t expire_at;
};

// The lease starts when its request was sent. T1 and T2 come from the server when they are in
// order, and are otherwise half and seven eighths of the lease. Zero seconds means the server
// gave no time; a missing or infinite lease never ends.
LeaseTimes ScheduleLease(uint64_t requested_at, uint32_t lease_seconds, uint32_t renewal_seconds,
                         uint32_t rebinding_seconds);

LeasePhase PhaseAt(const LeaseTimes& times, uint64_t now);

// When a request sent at now is retried: after half the time left until T2 while bound or
// renewing, or until expiry while rebinding, but at least 60 s later and never past that point.
uint64_t RetryAt(const LeaseTimes& times, uint64_t now);

// How long a discover waits after attempt earlier ones failed: 1 s, doubling up to 64 s.
uint64_t DiscoverTimeout(unsigned attempt);

} // namespace FeatherOS::User::Dhcp

#endif
