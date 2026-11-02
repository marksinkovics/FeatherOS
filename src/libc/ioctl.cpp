#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <termios.h>

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
        if (tcgetwinsize(descriptor, &unused) == 0)
        {
            errno = EINVAL;
        }
        return -1;
    }
    va_list arguments;
    va_start(arguments, request);
    auto* size = va_arg(arguments, struct winsize*);
    va_end(arguments);
    return request == TIOCGWINSZ ? tcgetwinsize(descriptor, size) : tcsetwinsize(descriptor, size);
}
