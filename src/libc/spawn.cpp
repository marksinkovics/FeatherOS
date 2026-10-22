#include "libc/spawn_request.h"
#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

constexpr int SignalInterrupt = 2;
constexpr int SignalSegmentationFault = 11;
constexpr uint64_t PollMilliseconds = 10;
// The kernel's limit on running children per process.
constexpr size_t MaximumChildren = 16;

// The children posix_spawn started and nobody has waited for yet, for waitpid(-1).
pid_t children[MaximumChildren];
size_t child_count = 0;

int ErrorFor(FeatherOS::LibC::SpawnRequestResult result)
{
    switch (result)
    {
    case FeatherOS::LibC::SpawnRequestResult::NotFound:
        return ENOENT;
    case FeatherOS::LibC::SpawnRequestResult::TooBig:
        return E2BIG;
    default:
        return EINVAL;
    }
}

void Forget(pid_t child)
{
    for (size_t index = 0; index < child_count; ++index)
    {
        if (children[index] == child)
        {
            children[index] = children[--child_count];
            return;
        }
    }
}

int EncodeStatus(uint64_t status)
{
    if (status == FeatherOS::User::ExitStatusFault ||
        status == FeatherOS::User::ExitStatusKernelAccessFault)
    {
        return SignalSegmentationFault;
    }
    if (status == FeatherOS::User::ExitStatusInterrupted)
    {
        return SignalInterrupt;
    }
    return static_cast<int>((status & 0xFF) << 8);
}

// 0 once the child is reaped, 1 if it still runs and no_hang was set, or -1 with errno.
int WaitFor(pid_t child, int* status, bool no_hang)
{
    uint64_t exit_status = 0;
    const uint64_t result = FeatherOS::User::Wait(static_cast<uint64_t>(child), &exit_status,
                                                  no_hang ? FeatherOS::User::WaitNoHang : 0);
    if (result == FeatherOS::User::SyscallErrorWouldBlock)
    {
        return 1;
    }
    if (FeatherOS::LibC::Check<int>(result) != 0)
    {
        return -1;
    }
    Forget(child);
    if (status != nullptr)
    {
        *status = EncodeStatus(exit_status);
    }
    return 0;
}

} // namespace

int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t*,
                const posix_spawnattr_t* attributes, char* const argv[], char* const envp[])
{
    uint64_t flags = 0;
    if (attributes != nullptr)
    {
        flags |=
            (attributes->flags & POSIX_SPAWN_SETSID) != 0 ? FeatherOS::User::SpawnSessionLeader : 0;
        flags |=
            (attributes->flags & POSIX_SPAWN_SETPGROUP) != 0 ? FeatherOS::User::SpawnBackground : 0;
    }
    char line[FeatherOS::User::MaximumCommandLineLength];
    char environment[FeatherOS::User::MaximumEnvironmentSize];
    FeatherOS::LibC::SpawnRequestResult built = FeatherOS::LibC::BuildCommandLine(path, argv, line);
    if (built == FeatherOS::LibC::SpawnRequestResult::Done)
    {
        built = FeatherOS::LibC::BuildEnvironmentBlock(envp, environment);
    }
    if (built != FeatherOS::LibC::SpawnRequestResult::Done)
    {
        return ErrorFor(built);
    }
    const uint64_t child = FeatherOS::User::Spawn(line, flags, environment);
    if (FeatherOS::User::IsSyscallError(child))
    {
        return FeatherOS::LibC::ErrorNumber(child);
    }
    if (child_count < MaximumChildren)
    {
        children[child_count++] = static_cast<pid_t>(child);
    }
    if (pid != nullptr)
    {
        *pid = static_cast<pid_t>(child);
    }
    return 0;
}

int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attributes, char* const argv[], char* const envp[])
{
    return posix_spawn(pid, file, file_actions, attributes, argv, envp);
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* file_actions)
{
    file_actions->unused = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*)
{
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t* attributes)
{
    attributes->flags = 0;
    attributes->process_group = 0;
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t*)
{
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t* attributes, short* flags)
{
    *flags = attributes->flags;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t* attributes, short flags)
{
    if ((flags & ~(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSID)) != 0)
    {
        return EINVAL;
    }
    attributes->flags = flags;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t* attributes, pid_t* process_group)
{
    *process_group = attributes->process_group;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t* attributes, pid_t process_group)
{
    if (process_group != 0)
    {
        return EINVAL;
    }
    attributes->process_group = process_group;
    return 0;
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    if ((options & ~WNOHANG) != 0 || pid == 0 || pid < -1)
    {
        errno = EINVAL;
        return -1;
    }
    const bool no_hang = (options & WNOHANG) != 0;
    if (pid > 0)
    {
        const int result = WaitFor(pid, status, no_hang);
        return result == 0 ? pid : result == 1 ? 0 : -1;
    }
    for (;;)
    {
        if (child_count == 0)
        {
            errno = ECHILD;
            return -1;
        }
        for (size_t index = 0; index < child_count;)
        {
            const pid_t child = children[index];
            const int result = WaitFor(child, status, true);
            if (result == 0)
            {
                return child;
            }
            // A child that is gone is dropped; Forget moves the last one into its slot.
            if (result < 0)
            {
                Forget(child);
                continue;
            }
            ++index;
        }
        if (no_hang)
        {
            return 0;
        }
        FeatherOS::User::Sleep(PollMilliseconds);
    }
}

pid_t wait(int* status)
{
    return waitpid(-1, status, 0);
}
