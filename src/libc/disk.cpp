#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <featheros/disk.h>
#include <string.h>

using FeatherOS::LibC::Check;

int eject(const char* disk)
{
    constexpr char Prefix[] = "/dev/";
    if (disk == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    if (strncmp(disk, Prefix, sizeof(Prefix) - 1) == 0)
    {
        disk += sizeof(Prefix) - 1;
    }
    return Check<int>(FeatherOS::User::Eject(disk));
}
