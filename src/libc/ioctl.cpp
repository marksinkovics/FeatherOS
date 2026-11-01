#include "syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{

int ReadSize(int descriptor, winsize* size)
{
    const uint64_t packed = FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::TerminalSize,
                                                     static_cast<uint64_t>(descriptor));
    if (FeatherOS::LibC::Check<int>(packed) < 0)
    {
        return -1;
    }
    size->ws_row = static_cast<unsigned short>(packed >> 32);
    size->ws_col = static_cast<unsigned short>(packed);
    size->ws_xpixel = 0;
    size->ws_ypixel = 0;
    return 0;
}

int WriteSize(int descriptor, const winsize* size)
{
    const uint64_t packed = (static_cast<uint64_t>(size->ws_row) << 32) | size->ws_col;
    return FeatherOS::LibC::Check<int>(FeatherOS::User::TerminalControl(
        static_cast<uint64_t>(descriptor), FeatherOS::User::TerminalControlSetSize, packed));
}

} // namespace

int ioctl(int descriptor, unsigned long request, ...)
{
    if (descriptor < 0)
    {
        errno = EBADF;
        return -1;
    }
    if (request != TIOCGWINSZ && request != TIOCSWINSZ)
    {
        // A terminal fails with EINVAL, anything else with ENOTTY.
        winsize unused;
        if (ReadSize(descriptor, &unused) == 0)
        {
            errno = EINVAL;
        }
        return -1;
    }
    va_list arguments;
    va_start(arguments, request);
    auto* size = va_arg(arguments, struct winsize*);
    va_end(arguments);
    return request == TIOCGWINSZ ? ReadSize(descriptor, size) : WriteSize(descriptor, size);
}
