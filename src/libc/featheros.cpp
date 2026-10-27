#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <featheros.h>
#include <stdio.h>

ssize_t featheros_read_line(const char* prompt, char* buffer, size_t size)
{
    if (size == 0)
    {
        errno = EINVAL;
        return -1;
    }
    fflush(stdout);
    return FeatherOS::LibC::Check<ssize_t>(
        FeatherOS::User::ReadLine(buffer, size, prompt == nullptr ? "" : prompt));
}
