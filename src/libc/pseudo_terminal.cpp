#include "syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>

using FeatherOS::LibC::Check;

namespace
{

uint64_t Control(int descriptor, uint64_t request, uint64_t argument)
{
    if (descriptor < 0)
    {
        return FeatherOS::User::SyscallErrorBadDescriptor;
    }
    return FeatherOS::User::TerminalControl(static_cast<uint64_t>(descriptor), request, argument);
}

} // namespace

int posix_openpt(int flags)
{
    if ((flags & ~(O_ACCMODE | O_NOCTTY | O_CLOEXEC)) != 0 || (flags & O_ACCMODE) != O_RDWR)
    {
        errno = EINVAL;
        return -1;
    }
    return open("/dev/ptmx", flags);
}

int grantpt(int descriptor)
{
    return Check<int>(Control(descriptor, FeatherOS::User::TerminalControlNumber, 0)) < 0 ? -1 : 0;
}

int unlockpt(int descriptor)
{
    return Check<int>(Control(descriptor, FeatherOS::User::TerminalControlLock, 0));
}

int ptsname_r(int descriptor, char* buffer, size_t capacity)
{
    const uint64_t number = Control(descriptor, FeatherOS::User::TerminalControlNumber, 0);
    if (FeatherOS::User::IsSyscallError(number))
    {
        return FeatherOS::LibC::ErrorNumber(number);
    }
    constexpr char Prefix[] = "/dev/pts/";
    char name[sizeof(Prefix) + 2];
    size_t length = 0;
    for (; Prefix[length] != '\0'; ++length)
    {
        name[length] = Prefix[length];
    }
    if (number >= 10)
    {
        name[length++] = static_cast<char>('0' + number / 10);
    }
    name[length++] = static_cast<char>('0' + number % 10);
    name[length] = '\0';
    if (buffer == nullptr || capacity <= length)
    {
        return ERANGE;
    }
    for (size_t index = 0; index <= length; ++index)
    {
        buffer[index] = name[index];
    }
    return 0;
}

char* ptsname(int descriptor)
{
    static char name[16];
    const int error = ptsname_r(descriptor, name, sizeof(name));
    if (error != 0)
    {
        errno = error;
        return nullptr;
    }
    return name;
}
