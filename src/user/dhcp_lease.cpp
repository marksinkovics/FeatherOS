#include "dhcp_lease.h"

namespace FeatherOS::User::Dhcp
{
namespace
{

constexpr uint64_t MinimumRetry = 60 * 1000;
constexpr uint64_t MaximumDiscoverTimeout = 64 * 1000;

uint64_t After(uint64_t start, uint64_t milliseconds)
{
    return milliseconds > Never - start ? Never : start + milliseconds;
}

} // namespace

LeaseTimes ScheduleLease(uint64_t requested_at, uint32_t lease_seconds, uint32_t renewal_seconds,
                         uint32_t rebinding_seconds)
{
    if (lease_seconds == 0 || lease_seconds == InfiniteLease)
    {
        return {.renew_at = Never, .rebind_at = Never, .expire_at = Never};
    }
    const uint64_t lease = static_cast<uint64_t>(lease_seconds) * 1000;
    const uint64_t rebind = rebinding_seconds != 0 && rebinding_seconds <= lease_seconds
                                ? static_cast<uint64_t>(rebinding_seconds) * 1000
                                : lease / 8 * 7;
    const uint64_t renew = renewal_seconds != 0 && renewal_seconds * 1000ULL <= rebind
                               ? static_cast<uint64_t>(renewal_seconds) * 1000
                           : lease / 2 <= rebind ? lease / 2
                                                 : rebind;
    return {.renew_at = After(requested_at, renew),
            .rebind_at = After(requested_at, rebind),
            .expire_at = After(requested_at, lease)};
}

LeasePhase PhaseAt(const LeaseTimes& times, uint64_t now)
{
    if (now >= times.expire_at)
    {
        return LeasePhase::Expired;
    }
    if (now >= times.rebind_at)
    {
        return LeasePhase::Rebinding;
    }
    return now >= times.renew_at ? LeasePhase::Renewing : LeasePhase::Bound;
}

uint64_t RetryAt(const LeaseTimes& times, uint64_t now)
{
    const uint64_t limit = now >= times.rebind_at ? times.expire_at : times.rebind_at;
    if (now >= limit)
    {
        return limit;
    }
    const uint64_t half = (limit - now) / 2;
    const uint64_t retry = After(now, half > MinimumRetry ? half : MinimumRetry);
    return retry < limit ? retry : limit;
}

uint64_t DiscoverTimeout(unsigned attempt)
{
    return attempt >= 6 ? MaximumDiscoverTimeout : 1000ULL << attempt;
}

} // namespace FeatherOS::User::Dhcp
