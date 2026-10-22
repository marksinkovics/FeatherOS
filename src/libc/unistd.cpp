#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

using FeatherOS::LibC::Check;

namespace
{

constexpr int KnownOpenFlags = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_CLOEXEC;

// The kernel starts an O_APPEND descriptor at the end of the file, but POSIX wants every write
// there, so write moves it first. Bit n stands for descriptor n.
uint32_t append_descriptors = 0;

bool IsAppending(int descriptor)
{
    return descriptor >= 0 && descriptor < 32 && (append_descriptors & (1U << descriptor)) != 0;
}

void SetAppending(int descriptor, bool appending)
{
    if (descriptor < 0 || descriptor >= 32)
    {
        return;
    }
    if (appending)
    {
        append_descriptors |= 1U << descriptor;
    }
    else
    {
        append_descriptors &= ~(1U << descriptor);
    }
}

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
    // Only one program can write a file at a time, so checking first leaves no race that
    // matters.
    struct stat status;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) && stat(path, &status) == 0)
    {
        errno = EEXIST;
        return -1;
    }
    const int descriptor = Check<int>(FeatherOS::User::OpenFile(path, open_flags));
    SetAppending(descriptor, descriptor >= 0 && (flags & O_APPEND) != 0);
    return descriptor;
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
    if (IsAppending(descriptor) && lseek(descriptor, 0, SEEK_END) < 0)
    {
        return -1;
    }
    return Check<ssize_t>(
        FeatherOS::User::WriteFile(static_cast<uint64_t>(descriptor), buffer, count));
}

int close(int descriptor)
{
    const int result = Check<int>(FeatherOS::User::CloseFile(static_cast<uint64_t>(descriptor)));
    if (result == 0)
    {
        SetAppending(descriptor, false);
    }
    return result;
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

int isatty(int descriptor)
{
    struct stat status;
    if (fstat(descriptor, &status) != 0)
    {
        return 0;
    }
    if (!S_ISCHR(status.st_mode))
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
    constexpr unsigned int SecondsPerDay = 86400;
    while (seconds > 0)
    {
        const unsigned int step = seconds < SecondsPerDay ? seconds : SecondsPerDay;
        FeatherOS::User::Sleep(static_cast<uint64_t>(step) * 1000);
        seconds -= step;
    }
    return 0;
}

int usleep(unsigned int microseconds)
{
    return Check<int>(FeatherOS::User::Sleep((static_cast<uint64_t>(microseconds) + 999) / 1000));
}

void* sbrk(intptr_t increment)
{
    if (increment < 0)
    {
        errno = EINVAL;
        return reinterpret_cast<void*>(-1);
    }
    const uint64_t previous = FeatherOS::User::ExtendHeap(static_cast<uint64_t>(increment));
    if (FeatherOS::User::IsSyscallError(previous))
    {
        errno = ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
    return reinterpret_cast<void*>(previous);
}

int sched_getcpu()
{
    return static_cast<int>(FeatherOS::User::CurrentCpu());
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

pid_t getppid()
{
    return static_cast<pid_t>(FeatherOS::User::ParentProcessId());
}

void _exit(int status)
{
    _Exit(status);
}
