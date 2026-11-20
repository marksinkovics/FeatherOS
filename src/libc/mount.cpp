#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mount.h>

using FeatherOS::LibC::Check;

int mount(const char* source, const char* target, const char* filesystemtype,
          unsigned long mountflags, const void* data)
{
    constexpr unsigned long Known = MS_RDONLY | MS_REMOUNT;
    const bool remount = (mountflags & MS_REMOUNT) != 0;
    if (target == nullptr || (mountflags & ~Known) != 0 || data != nullptr ||
        (!remount && (source == nullptr || filesystemtype == nullptr)))
    {
        errno = EINVAL;
        return -1;
    }
    uint64_t flags = (mountflags & MS_RDONLY) != 0 ? FeatherOS::User::MountReadOnly : 0;
    if (remount)
    {
        flags |= FeatherOS::User::MountRemount;
    }
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
