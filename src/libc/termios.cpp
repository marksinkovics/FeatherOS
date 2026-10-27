#include "syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

using FeatherOS::LibC::Check;

int tcgetattr(int descriptor, struct termios* settings)
{
    if (!isatty(descriptor))
    {
        return -1;
    }
    FeatherOS::User::TerminalSettings current;
    if (Check<int>(FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::TerminalMode, 0,
                                            reinterpret_cast<uint64_t>(&current))) < 0)
    {
        return -1;
    }
    settings->c_iflag =
        (current.flags & FeatherOS::User::TerminalCarriageReturnToNewline) != 0 ? ICRNL : 0;
    settings->c_oflag = OPOST;
    settings->c_cflag = CS8;
    settings->c_lflag = ((current.flags & FeatherOS::User::TerminalCanonical) != 0 ? ICANON : 0) |
                        ((current.flags & FeatherOS::User::TerminalEcho) != 0 ? ECHO : 0) |
                        ((current.flags & FeatherOS::User::TerminalSignals) != 0 ? ISIG : 0);
    for (cc_t& character : settings->c_cc)
    {
        character = 0;
    }
    settings->c_cc[VINTR] = 3;
    settings->c_cc[VERASE] = 0x7F;
    settings->c_cc[VEOF] = 4;
    settings->c_cc[VMIN] = current.minimum;
    settings->c_cc[VTIME] = current.timeout;
    return 0;
}

int tcsetattr(int descriptor, int when, const struct termios* settings)
{
    if (!isatty(descriptor))
    {
        return -1;
    }
    if (when != TCSANOW && when != TCSADRAIN && when != TCSAFLUSH)
    {
        errno = EINVAL;
        return -1;
    }
    FeatherOS::User::TerminalSettings requested;
    requested.flags =
        ((settings->c_iflag & ICRNL) != 0 ? FeatherOS::User::TerminalCarriageReturnToNewline : 0) |
        ((settings->c_lflag & ICANON) != 0 ? FeatherOS::User::TerminalCanonical : 0) |
        ((settings->c_lflag & ECHO) != 0 ? FeatherOS::User::TerminalEcho : 0) |
        ((settings->c_lflag & ISIG) != 0 ? FeatherOS::User::TerminalSignals : 0);
    // Canonical reads ignore the minimum and timeout, so they keep the defaults there.
    const bool canonical = (settings->c_lflag & ICANON) != 0;
    requested.minimum = canonical ? 1 : settings->c_cc[VMIN];
    requested.timeout = canonical ? 0 : settings->c_cc[VTIME];
    requested.reserved = 0;
    return Check<int>(FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::TerminalMode,
                                               reinterpret_cast<uint64_t>(&requested), 0)) < 0
               ? -1
               : 0;
}

void cfmakeraw(struct termios* settings)
{
    settings->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    settings->c_oflag &= ~OPOST;
    settings->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    settings->c_cflag &= ~(CSIZE | PARENB);
    settings->c_cflag |= CS8;
    settings->c_cc[VMIN] = 1;
    settings->c_cc[VTIME] = 0;
}
