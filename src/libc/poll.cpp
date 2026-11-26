#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <poll.h>

static_assert(sizeof(pollfd) == sizeof(FeatherOS::User::PollDescriptor));
static_assert(POLLIN == FeatherOS::User::PollReadable && POLLOUT == FeatherOS::User::PollWritable &&
              POLLERR == FeatherOS::User::PollError && POLLHUP == FeatherOS::User::PollHangUp &&
              POLLNVAL == FeatherOS::User::PollInvalid);

// The kernel times out in whole ticks from the next one, which can be a tick before the monotonic
// clock says the time is up, so an empty poll goes back for what is left.
int poll(struct pollfd* descriptors, nfds_t count, int timeout)
{
    constexpr uint64_t NanosecondsPerMillisecond = 1000000;
    auto* kernel_descriptors = reinterpret_cast<FeatherOS::User::PollDescriptor*>(descriptors);
    if (timeout <= 0)
    {
        return FeatherOS::LibC::Check<int>(FeatherOS::User::PollDescriptors(
            kernel_descriptors, count,
            timeout < 0 ? FeatherOS::User::PollForever : static_cast<uint64_t>(timeout)));
    }
    const uint64_t start = FeatherOS::User::ReadMonotonic();
    const uint64_t requested = static_cast<uint64_t>(timeout) * NanosecondsPerMillisecond;
    uint64_t milliseconds = static_cast<uint64_t>(timeout);
    while (true)
    {
        const uint64_t result =
            FeatherOS::User::PollDescriptors(kernel_descriptors, count, milliseconds);
        const uint64_t now = FeatherOS::User::ReadMonotonic();
        if (result != 0 || FeatherOS::User::IsSyscallError(start) ||
            FeatherOS::User::IsSyscallError(now) || now - start >= requested)
        {
            return FeatherOS::LibC::Check<int>(result);
        }
        milliseconds =
            (requested - (now - start) + NanosecondsPerMillisecond - 1) / NanosecondsPerMillisecond;
    }
}
