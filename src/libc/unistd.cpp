#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <stdlib.h>
#include <unistd.h>

using FeatherOS::LibC::Check;

ssize_t read(int descriptor, void* buffer, size_t count)
{
    return Check<ssize_t>(
        FeatherOS::User::ReadFile(static_cast<uint64_t>(descriptor), buffer, count));
}

ssize_t write(int descriptor, const void* buffer, size_t count)
{
    return Check<ssize_t>(
        FeatherOS::User::WriteFile(static_cast<uint64_t>(descriptor), buffer, count));
}

int close(int descriptor)
{
    return Check<int>(FeatherOS::User::CloseFile(static_cast<uint64_t>(descriptor)));
}

pid_t getpid()
{
    return static_cast<pid_t>(FeatherOS::User::ProcessId());
}

pid_t getppid()
{
    return static_cast<pid_t>(FeatherOS::User::ParentProcessId());
}

void _exit(int status)
{
    _Exit(status);
}
