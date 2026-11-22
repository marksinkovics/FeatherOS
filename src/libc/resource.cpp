#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <sys/resource.h>

using FeatherOS::LibC::Check;
using FeatherOS::User::ResourceLimits;

static_assert(sizeof(rlimit) == sizeof(ResourceLimits) && sizeof(rlim_t) == sizeof(uint64_t));

int getrlimit(int resource, struct rlimit* limits)
{
    if (limits == nullptr)
    {
        errno = EFAULT;
        return -1;
    }
    return Check<int>(FeatherOS::User::ResourceLimit(static_cast<uint64_t>(resource), nullptr,
                                                     reinterpret_cast<ResourceLimits*>(limits)));
}

int setrlimit(int resource, const struct rlimit* limits)
{
    if (limits == nullptr)
    {
        errno = EFAULT;
        return -1;
    }
    return Check<int>(FeatherOS::User::ResourceLimit(
        static_cast<uint64_t>(resource), reinterpret_cast<const ResourceLimits*>(limits), nullptr));
}
