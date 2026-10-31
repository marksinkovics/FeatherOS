#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <poll.h>

static_assert(sizeof(pollfd) == sizeof(FeatherOS::User::PollDescriptor));
static_assert(POLLIN == FeatherOS::User::PollReadable && POLLOUT == FeatherOS::User::PollWritable &&
              POLLERR == FeatherOS::User::PollError && POLLHUP == FeatherOS::User::PollHangUp &&
              POLLNVAL == FeatherOS::User::PollInvalid);

int poll(struct pollfd* descriptors, nfds_t count, int timeout)
{
    return FeatherOS::LibC::Check<int>(FeatherOS::User::PollDescriptors(
        reinterpret_cast<FeatherOS::User::PollDescriptor*>(descriptors), count,
        timeout < 0 ? FeatherOS::User::PollForever : static_cast<uint64_t>(timeout)));
}
