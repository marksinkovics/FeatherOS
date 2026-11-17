#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mount.h>

using FeatherOS::LibC::Check;

int mount(const char* source, const char* target, const char* filesystemtype,
          unsigned long mountflags, const void* data)
{
    if (source == nullptr || target == nullptr || filesystemtype == nullptr ||
        (mountflags & ~static_cast<unsigned long>(MS_RDONLY)) != 0 || data != nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    const uint64_t flags = (mountflags & MS_RDONLY) != 0 ? FeatherOS::User::MountReadOnly : 0;
    return Check<int>(FeatherOS::User::Mount(source, target, filesystemtype, flags));
}

int umount(const char* target)
{
    if (target == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    return Check<int>(FeatherOS::User::Unmount(target));
}
