#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using FeatherOS::LibC::Check;

namespace
{

constexpr int KnownOpenFlags =
    O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_CLOEXEC | O_NONBLOCK;

} // namespace

int open(const char* path, int flags, ...)
{
    const int access_mode = flags & O_ACCMODE;
    if ((flags & ~KnownOpenFlags) != 0 || access_mode == O_ACCMODE)
    {
        errno = EINVAL;
        return -1;
    }
    uint64_t open_flags = 0;
    if (access_mode != O_WRONLY)
    {
        open_flags |= FeatherOS::User::OpenRead;
    }
    if (access_mode != O_RDONLY)
    {
        open_flags |= FeatherOS::User::OpenWrite;
    }
    if ((flags & O_CREAT) != 0)
    {
        open_flags |= FeatherOS::User::OpenCreate;
    }
    if ((flags & O_TRUNC) != 0)
    {
        open_flags |= FeatherOS::User::OpenTruncate;
    }
    if ((flags & O_APPEND) != 0)
    {
        open_flags |= FeatherOS::User::OpenAppend;
    }
    if ((flags & O_CLOEXEC) != 0)
    {
        open_flags |= FeatherOS::User::OpenCloseOnExec;
    }
    if ((flags & O_NONBLOCK) != 0)
    {
        open_flags |= FeatherOS::User::OpenNonBlocking;
    }
    // Only one program can write a file at a time, so checking first leaves no race that
    // matters.
    struct stat status;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) && stat(path, &status) == 0)
    {
        errno = EEXIST;
        return -1;
    }
    return Check<int>(FeatherOS::User::OpenFile(path, open_flags));
}

int creat(const char* path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

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

int pipe(int descriptors[2])
{
    return pipe2(descriptors, 0);
}

int pipe2(int descriptors[2], int flags)
{
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    uint64_t pipe_flags = 0;
    if ((flags & O_CLOEXEC) != 0)
    {
        pipe_flags |= FeatherOS::User::PipeCloseOnExec;
    }
    if ((flags & O_NONBLOCK) != 0)
    {
        pipe_flags |= FeatherOS::User::PipeNonBlocking;
    }
    uint64_t ends[2];
    if (Check<int>(FeatherOS::User::CreatePipe(ends, pipe_flags)) < 0)
    {
        return -1;
    }
    descriptors[0] = static_cast<int>(ends[0]);
    descriptors[1] = static_cast<int>(ends[1]);
    return 0;
}

int dup(int descriptor)
{
    return Check<int>(FeatherOS::User::Duplicate(static_cast<uint64_t>(descriptor), 0, 0));
}

int dup2(int descriptor, int target)
{
    if (target < 0)
    {
        errno = EBADF;
        return -1;
    }
    return Check<int>(FeatherOS::User::Duplicate(static_cast<uint64_t>(descriptor),
                                                 static_cast<uint64_t>(target),
                                                 FeatherOS::User::DuplicateExact));
}

int fcntl(int descriptor, int command, ...)
{
    va_list arguments;
    va_start(arguments, command);
    const int argument = command == F_GETFD || command == F_GETFL ? 0 : va_arg(arguments, int);
    va_end(arguments);
    const auto target = static_cast<uint64_t>(descriptor);
    switch (command)
    {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if (argument < 0)
        {
            errno = EINVAL;
            return -1;
        }
        return Check<int>(FeatherOS::User::Duplicate(
            target, static_cast<uint64_t>(argument),
            command == F_DUPFD_CLOEXEC ? FeatherOS::User::DuplicateCloseOnExec : 0));
    case F_GETFD:
    case F_SETFD:
    {
        const uint64_t requested =
            (argument & FD_CLOEXEC) != 0 ? FeatherOS::User::DescriptorCloseOnExec : 0;
        const int flags =
            Check<int>(FeatherOS::User::DescriptorFlags(target, command == F_SETFD, requested));
        if (flags < 0 || command == F_SETFD)
        {
            return flags < 0 ? -1 : 0;
        }
        return (static_cast<uint64_t>(flags) & FeatherOS::User::DescriptorCloseOnExec) != 0
                   ? FD_CLOEXEC
                   : 0;
    }
    case F_GETFL:
    case F_SETFL:
    {
        uint64_t requested = 0;
        if ((argument & O_APPEND) != 0)
        {
            requested |= FeatherOS::User::OpenAppend;
        }
        if ((argument & O_NONBLOCK) != 0)
        {
            requested |= FeatherOS::User::OpenNonBlocking;
        }
        const uint64_t result =
            FeatherOS::User::FileStatusFlags(target, command == F_SETFL, requested);
        if (Check<int>(result) < 0)
        {
            return -1;
        }
        if (command == F_SETFL)
        {
            return 0;
        }
        const bool readable = (result & FeatherOS::User::OpenRead) != 0;
        const bool writable = (result & FeatherOS::User::OpenWrite) != 0;
        int flags = readable && writable ? O_RDWR : (writable ? O_WRONLY : O_RDONLY);
        if ((result & FeatherOS::User::OpenAppend) != 0)
        {
            flags |= O_APPEND;
        }
        if ((result & FeatherOS::User::OpenNonBlocking) != 0)
        {
            flags |= O_NONBLOCK;
        }
        return flags;
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

off_t lseek(int descriptor, off_t offset, int whence)
{
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
    {
        errno = EINVAL;
        return -1;
    }
    return Check<off_t>(FeatherOS::User::SeekFile(static_cast<uint64_t>(descriptor),
                                                  static_cast<uint64_t>(offset),
                                                  static_cast<uint64_t>(whence)));
}

int unlink(const char* path)
{
    return Check<int>(FeatherOS::User::RemoveFile(path));
}

int rmdir(const char* path)
{
    return Check<int>(FeatherOS::User::RemoveDirectory(path));
}

int mkdir(const char* path, mode_t)
{
    return Check<int>(FeatherOS::User::MakeDirectory(path));
}

int chdir(const char* path)
{
    return Check<int>(FeatherOS::User::ChangeDirectory(path));
}

// A null buffer is not supported; POSIX leaves allocating one to the implementation.
char* getcwd(char* buffer, size_t size)
{
    if (buffer == nullptr || size == 0)
    {
        errno = EINVAL;
        return nullptr;
    }
    return Check<int64_t>(FeatherOS::User::CurrentDirectory(buffer, size)) < 0 ? nullptr : buffer;
}

int access(const char* path, int mode)
{
    struct stat status;
    if (stat(path, &status) != 0)
    {
        return -1;
    }
    if ((mode & W_OK) != 0 && (status.st_mode & 0200) == 0)
    {
        errno = EROFS;
        return -1;
    }
    return 0;
}

// Devices under /dev are character devices too, so the kernel's file type tells the console
// apart.
int isatty(int descriptor)
{
    FeatherOS::User::FileStatus status{};
    if (descriptor < 0 ||
        Check<int>(FeatherOS::User::StatDescriptor(static_cast<uint64_t>(descriptor), &status)) < 0)
    {
        return 0;
    }
    if (status.type != FeatherOS::User::FileType::Console)
    {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

// The kernel's Sleep is never cut short, so the whole time always passes. It takes at most a day
// per call.
unsigned int sleep(unsigned int seconds)
{
    struct timespec duration = {static_cast<time_t>(seconds), 0};
    struct timespec remaining = {0, 0};
    if (nanosleep(&duration, &remaining) == 0)
    {
        return 0;
    }
    // Rounded up, so a sleep cut short never reports that no time was left.
    return static_cast<unsigned int>(remaining.tv_sec) + (remaining.tv_nsec > 0 ? 1 : 0);
}

int usleep(unsigned int microseconds)
{
    return Check<int>(FeatherOS::User::Sleep((static_cast<uint64_t>(microseconds) + 999) / 1000));
}

// The kernel takes a negative increment as its two's-complement value. Shrinking below the start
// of the heap fails with EINVAL, and growing past the page limit with ENOMEM.
void* sbrk(intptr_t increment)
{
    const uint64_t previous = FeatherOS::User::ExtendHeap(static_cast<uint64_t>(increment));
    if (FeatherOS::User::IsSyscallError(previous))
    {
        errno = increment < 0 ? EINVAL : ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
    return reinterpret_cast<void*>(previous);
}

int sched_getcpu()
{
    return static_cast<int>(FeatherOS::User::CurrentCpu());
}

int sched_yield()
{
    FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::Sleep, 0);
    return 0;
}

int reboot(int command)
{
    if (command != RB_AUTOBOOT && command != RB_POWER_OFF)
    {
        errno = EINVAL;
        return -1;
    }
    return Check<int>(FeatherOS::User::Shutdown(command == RB_POWER_OFF
                                                    ? FeatherOS::User::ShutdownPowerOff
                                                    : FeatherOS::User::ShutdownRestart));
}

pid_t getpid()
{
    return static_cast<pid_t>(FeatherOS::User::ProcessId());
}

long sysconf(int name)
{
    if (name == _SC_OPEN_MAX)
    {
        return static_cast<long>(FeatherOS::User::MaximumDescriptors);
    }
    if (name == _SC_PAGESIZE)
    {
        return 4096;
    }
    if (name != _SC_NPROCESSORS_CONF && name != _SC_NPROCESSORS_ONLN)
    {
        errno = EINVAL;
        return -1;
    }
    // Every CPU the kernel started is online, so both counts are the same.
    const int descriptor = open("/proc/stat", O_RDONLY);
    char text[256];
    const ssize_t length = descriptor < 0 ? -1 : read(descriptor, text, sizeof(text) - 1);
    if (descriptor >= 0)
    {
        close(descriptor);
    }
    if (length <= 0)
    {
        return -1;
    }
    text[length] = '\0';
    const char* cpus = strstr(text, "Cpus:\t");
    return cpus == nullptr ? -1 : strtol(cpus + 6, nullptr, 10);
}

pid_t getpgrp()
{
    return static_cast<pid_t>(FeatherOS::User::ProcessGroup(0));
}

pid_t getpgid(pid_t pid)
{
    if (pid < 0)
    {
        errno = EINVAL;
        return -1;
    }
    const uint64_t group = FeatherOS::User::ProcessGroup(static_cast<uint64_t>(pid));
    if (group == FeatherOS::User::SyscallErrorNotFound)
    {
        errno = ESRCH;
        return -1;
    }
    return static_cast<pid_t>(group);
}

// The console is the only terminal.
pid_t tcgetpgrp(int descriptor)
{
    if (!isatty(descriptor))
    {
        return -1;
    }
    return static_cast<pid_t>(FeatherOS::User::ForegroundGroup(false, 0));
}

int tcsetpgrp(int descriptor, pid_t group)
{
    if (!isatty(descriptor))
    {
        return -1;
    }
    if (group <= 0)
    {
        errno = EINVAL;
        return -1;
    }
    return Check<int>(FeatherOS::User::ForegroundGroup(true, static_cast<uint64_t>(group))) < 0 ? -1
                                                                                                : 0;
}

pid_t getppid()
{
    return static_cast<pid_t>(FeatherOS::User::ParentProcessId());
}

void _exit(int status)
{
    _Exit(status);
}
