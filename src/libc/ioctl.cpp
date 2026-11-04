#include <errno.h>
#include <featheros.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <termios.h>

int featheros_ioctl(int descriptor, unsigned long request, void* argument)
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
    auto* size = static_cast<struct winsize*>(argument);
    return request == TIOCGWINSZ ? tcgetwinsize(descriptor, size) : tcsetwinsize(descriptor, size);
}

int ioctl(int descriptor, unsigned long request, ...)
{
    void* argument = nullptr;
    if (request == TIOCGWINSZ || request == TIOCSWINSZ)
    {
        va_list arguments;
        va_start(arguments, request);
        argument = va_arg(arguments, void*);
        va_end(arguments);
    }
    return featheros_ioctl(descriptor, request, argument);
}
