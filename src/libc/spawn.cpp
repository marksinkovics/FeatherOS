#include "libc/spawn_request.h"
#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

constexpr int SignalSegmentationFault = 11;
constexpr uint64_t PollMilliseconds = 10;
// posix_spawnp's search list when PATH is not set.
constexpr char DefaultSearchPath[] = "/system:/bin";
// The kernel's limit on running children per process.
constexpr size_t MaximumChildren = 16;

enum FileActionKind
{
    ActionDuplicate,
    ActionOpen,
    ActionClose,
};
// Copies of the caller's descriptors that file actions replace sit at or above this one, out
// of the way of the low numbers the actions usually name.
constexpr int SavedDescriptorBase = 24;

struct SavedDescriptor
{
    int descriptor;
    // -1 if the descriptor was closed.
    int copy;
};

// Keeps the first state of each descriptor the actions touch, so it can be put back.
struct SavedDescriptors
{
    SavedDescriptor entries[POSIX_SPAWN_MAXIMUM_FILE_ACTIONS];
    int count = 0;

    bool Save(int descriptor)
    {
        for (int index = 0; index < count; ++index)
        {
            if (entries[index].descriptor == descriptor)
            {
                return true;
            }
        }
        const int copy = fcntl(descriptor, F_DUPFD_CLOEXEC, SavedDescriptorBase);
        if (copy < 0 && errno != EBADF)
        {
            return false;
        }
        entries[count++] = {descriptor, copy};
        return true;
    }

    void Restore()
    {
        const int error = errno;
        for (int index = count - 1; index >= 0; --index)
        {
            const SavedDescriptor& saved = entries[index];
            if (saved.copy >= 0)
            {
                dup2(saved.copy, saved.descriptor);
                close(saved.copy);
            }
            else
            {
                close(saved.descriptor);
            }
        }
        count = 0;
        errno = error;
    }
};

// Returns 0 or an errno value; the caller restores the saved descriptors either way.
int ApplyFileActions(const posix_spawn_file_actions_t& file_actions, SavedDescriptors* saved)
{
    for (int index = 0; index < file_actions.count; ++index)
    {
        const __posix_spawn_file_action& action = file_actions.actions[index];
        if (!saved->Save(action.descriptor))
        {
            return errno;
        }
        if (action.kind == ActionDuplicate && dup2(action.source, action.descriptor) < 0)
        {
            return errno;
        }
        if (action.kind == ActionClose)
        {
            close(action.descriptor);
        }
        if (action.kind != ActionOpen)
        {
            continue;
        }
        const int opened = open(action.path, action.flags & ~O_CLOEXEC);
        if (opened < 0)
        {
            return errno;
        }
        if (opened != action.descriptor)
        {
            const bool moved = dup2(opened, action.descriptor) >= 0;
            close(opened);
            if (!moved)
            {
                return errno;
            }
        }
    }
    return 0;
}

int AddFileAction(posix_spawn_file_actions_t* file_actions, const __posix_spawn_file_action& action)
{
    if (action.descriptor < 0 || action.source < 0)
    {
        return EBADF;
    }
    if (file_actions->count == POSIX_SPAWN_MAXIMUM_FILE_ACTIONS)
    {
        return ENOMEM;
    }
    file_actions->actions[file_actions->count++] = action;
    return 0;
}

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
    if (FeatherOS::User::IsSignalExit(status))
    {
        return static_cast<int>(status - FeatherOS::User::ExitStatusSignalBase);
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

int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attributes, char* const argv[], char* const envp[])
{
    uint64_t flags = 0;
    if (attributes != nullptr)
    {
        flags |=
            (attributes->flags & POSIX_SPAWN_SETSID) != 0 ? FeatherOS::User::SpawnSessionLeader : 0;
        flags |= (attributes->flags & POSIX_SPAWN_BACKGROUND) != 0
                     ? FeatherOS::User::SpawnBackground
                     : 0;
        if ((attributes->flags & POSIX_SPAWN_SETPGROUP) != 0)
        {
            flags |=
                FeatherOS::User::SpawnSetGroup | (static_cast<uint64_t>(attributes->process_group)
                                                  << FeatherOS::User::SpawnGroupShift);
        }
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
    SavedDescriptors saved;
    if (file_actions != nullptr)
    {
        const int error = ApplyFileActions(*file_actions, &saved);
        if (error != 0)
        {
            saved.Restore();
            return error;
        }
    }
    const uint64_t child = FeatherOS::User::Spawn(line, flags, environment);
    saved.Restore();
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
    if (*file == '\0' || strchr(file, '/') != nullptr)
    {
        return posix_spawn(pid, file, file_actions, attributes, argv, envp);
    }
    const char* search = getenv("PATH");
    if (search == nullptr)
    {
        search = DefaultSearchPath;
    }
    const int error = errno;
    char candidate[FeatherOS::User::MaximumPathLength];
    while (FeatherOS::LibC::NextSearchCandidate(&search, file, candidate))
    {
        struct stat status;
        if (stat(candidate, &status) == 0 && S_ISREG(status.st_mode))
        {
            errno = error;
            return posix_spawn(pid, candidate, file_actions, attributes, argv, envp);
        }
    }
    errno = error;
    return ENOENT;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* file_actions)
{
    file_actions->count = 0;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* file_actions, int descriptor,
                                     int target)
{
    return AddFileAction(file_actions, {ActionDuplicate, target, descriptor, 0, {}});
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* file_actions, int descriptor,
                                     const char* path, int flags, mode_t)
{
    __posix_spawn_file_action action = {ActionOpen, descriptor, 0, flags, {}};
    const size_t length = strlen(path);
    if (length >= sizeof(action.path))
    {
        return ENAMETOOLONG;
    }
    memcpy(action.path, path, length + 1);
    return AddFileAction(file_actions, action);
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* file_actions, int descriptor)
{
    return AddFileAction(file_actions, {ActionClose, descriptor, 0, 0, {}});
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
    if ((flags & ~(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSID | POSIX_SPAWN_BACKGROUND)) != 0)
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
    if (process_group < 0)
    {
        return EINVAL;
    }
    attributes->process_group = process_group;
    return 0;
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    if ((options & ~WNOHANG) != 0)
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
    // 0 means the caller's own group, and -n group n; -1 means any child.
    const pid_t group = pid == 0 ? getpgrp() : pid < -1 ? -pid : 0;
    for (;;)
    {
        bool any_child = false;
        for (size_t index = 0; index < child_count;)
        {
            const pid_t child = children[index];
            if (group != 0 && getpgid(child) != group)
            {
                ++index;
                continue;
            }
            any_child = true;
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
        if (!any_child)
        {
            errno = ECHILD;
            return -1;
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
