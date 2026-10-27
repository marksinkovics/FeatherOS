#include "user/syscall.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

int ioctl(int descriptor, unsigned long request, ...)
{
    if (!isatty(descriptor))
    {
        errno = ENOTTY;
        return -1;
    }
    if (request != TIOCGWINSZ)
    {
        errno = EINVAL;
        return -1;
    }
    va_list arguments;
    va_start(arguments, request);
    auto* size = va_arg(arguments, struct winsize*);
    va_end(arguments);
    const uint64_t packed = FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::TerminalSize);
    size->ws_row = static_cast<unsigned short>(packed >> 32);
    size->ws_col = static_cast<unsigned short>(packed);
    size->ws_xpixel = 0;
    size->ws_ypixel = 0;
    return 0;
}
