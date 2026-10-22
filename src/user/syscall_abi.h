#ifndef FEATHEROS_USER_SYSCALL_ABI_H
#define FEATHEROS_USER_SYSCALL_ABI_H

#include <stdint.h>

// Syscall numbers and result codes shared by the kernel and user programs.
// x86-64 passes the number in rax and arguments in rdi, rsi, rdx; AArch64 uses x8 and x0-x2.
namespace FeatherOS::User
{

enum class SyscallNumber : uint64_t
{
    Exit = 0,              // (status) -> does not return
    WriteCharacter = 1,    // (character) -> 0; printable ASCII or '\n'
    ReadLine = 2,          // (buffer, capacity, prompt) -> line length; foreground process only
    Spawn = 3,             // (command_line, flags, environment) -> process ID of the child
    Wait = 4,              // (process_id, status_pointer, flags) -> 0 once the child has exited
    ReadClock = 5,         // () -> current UTC time in seconds since 1970-01-01
    ExtendHeap = 6,        // (signed increment) -> previous program break; 0 queries it
    Sleep = 7,             // (milliseconds) -> 0 after at least that long
    ReadUptime = 8,        // () -> milliseconds since the kernel's timer started
    CurrentCpu = 9,        // () -> index of the CPU running the caller
    Open = 10,             // (path, flags) -> file descriptor
    Read = 11,             // (descriptor, buffer, length) -> bytes read; 0 at end of file
    Write = 12,            // (descriptor, buffer, length) -> bytes written
    Close = 13,            // (descriptor) -> 0
    Seek = 14,             // (descriptor, offset, whence) -> the new offset, at most the size
    Remove = 15,           // (path) -> 0
    ReadDirectory = 16,    // (path, index, entry) -> 0; SyscallErrorNotFound past the end
    MakeDirectory = 17,    // (path) -> 0
    RemoveDirectory = 18,  // (path) -> 0; the directory must be empty
    ChangeDirectory = 19,  // (path) -> 0
    CurrentDirectory = 20, // (buffer, capacity) -> length of the working directory
    Shutdown = 21,         // (action) -> does not return; init and session leaders only
    Stat = 22,             // (path, status) -> 0
    StatDescriptor = 23,   // (descriptor, status) -> 0
    ProcessId = 24,        // () -> the caller's process ID
    ParentProcessId = 25,  // () -> the parent's process ID; 0 for init
};

// Spawn flags. A child normally takes the console from its parent; a background child does not.
// A session leader's descendants form Ctrl+C jobs, and the leader itself is never interrupted.
constexpr uint64_t SpawnBackground = 1U << 0;
constexpr uint64_t SpawnSessionLeader = 1U << 1;
// Shutdown actions.
constexpr uint64_t ShutdownPowerOff = 0;
constexpr uint64_t ShutdownRestart = 1;
// Wait flag: return SyscallErrorWouldBlock instead of blocking while the child runs.
constexpr uint64_t WaitNoHang = 1U << 0;
// Seek origins; the offset is signed and added to the origin.
constexpr uint64_t SeekFromStart = 0;
constexpr uint64_t SeekFromCurrent = 1;
constexpr uint64_t SeekFromEnd = 2;

// Spawn's environment is "NAME=value" strings, each with its terminator, then an empty string.
// A null pointer gives the child an empty environment.
constexpr uint64_t MaximumEnvironmentSize = 1024;
constexpr uint64_t MaximumEnvironmentVariables = 32;

// Paths are absolute ("/home/notes.txt") or relative to the process's working directory, which
// starts as its parent's ("/home" for the shell). "/" is the disk; "/system" is the read-only
// initramfs.
constexpr uint64_t OpenRead = 1U << 0;
constexpr uint64_t OpenWrite = 1U << 1;
// With OpenWrite: create the file if it is missing, or empty it if it exists.
constexpr uint64_t OpenCreate = 1U << 2;
constexpr uint64_t OpenTruncate = 1U << 3;
// With OpenWrite: start at the end of the file.
constexpr uint64_t OpenAppend = 1U << 4;
constexpr uint64_t MaximumPathLength = 64;
// Descriptors 0-2 are the console in every process. Each is readable and writable, like a
// terminal: reading returns one line, ending in '\n', and only the foreground process may read.
constexpr uint64_t StandardInput = 0;
constexpr uint64_t StandardOutput = 1;
constexpr uint64_t StandardError = 2;
constexpr uint64_t FirstFileDescriptor = 3;
constexpr uint64_t MaximumOpenFiles = 16;

struct DirectoryEntry
{
    // The long name if the file has one, otherwise its 8.3 name, and a terminator.
    char name[64];
    bool directory;
    uint64_t size;
};

enum class FileType : uint32_t
{
    Regular = 1,
    Directory = 2,
    Console = 3,
};

struct FileStatus
{
    uint64_t size;
    FileType type;
    // The initramfs under /system cannot be changed.
    bool read_only;
};

// Results at or above SyscallErrorBase report failures instead of values. As on Linux, the top
// 4095 values are reserved, which no size, address, or process ID ever reaches.
constexpr uint64_t SyscallErrorBase = UINT64_MAX - 4094;
constexpr uint64_t SyscallErrorFailed = UINT64_MAX;
constexpr uint64_t SyscallErrorNotFound = UINT64_MAX - 1;
constexpr uint64_t SyscallErrorInvalidArgument = UINT64_MAX - 2;
constexpr uint64_t SyscallErrorNotForeground = UINT64_MAX - 3;
constexpr uint64_t SyscallErrorInterrupted = UINT64_MAX - 4;
constexpr uint64_t SyscallErrorTooManyOpenFiles = UINT64_MAX - 5;
// A disk file has either one writer or any number of readers, and an open file cannot be removed.
constexpr uint64_t SyscallErrorBusy = UINT64_MAX - 6;
constexpr uint64_t SyscallErrorWouldBlock = UINT64_MAX - 7;
constexpr uint64_t SyscallErrorNotPermitted = UINT64_MAX - 8;
// The descriptor is not open, or not open for that kind of access.
constexpr uint64_t SyscallErrorBadDescriptor = UINT64_MAX - 9;
// A pointer argument is not mapped, or not writable where the kernel must write.
constexpr uint64_t SyscallErrorBadAddress = UINT64_MAX - 10;
constexpr uint64_t SyscallErrorReadOnly = UINT64_MAX - 11;
constexpr uint64_t SyscallErrorExists = UINT64_MAX - 12;
constexpr uint64_t SyscallErrorNotDirectory = UINT64_MAX - 13;
constexpr uint64_t SyscallErrorNotEmpty = UINT64_MAX - 14;
// The console cannot seek.
constexpr uint64_t SyscallErrorNotSeekable = UINT64_MAX - 15;
constexpr uint64_t SyscallErrorNameTooLong = UINT64_MAX - 16;
constexpr uint64_t SyscallErrorIsDirectory = UINT64_MAX - 17;
// Wait names a process that is not the caller's child.
constexpr uint64_t SyscallErrorNotChild = UINT64_MAX - 18;
constexpr uint64_t SyscallErrorNoMemory = UINT64_MAX - 19;
constexpr uint64_t SyscallErrorNotExecutable = UINT64_MAX - 20;
// The result does not fit the caller's buffer.
constexpr uint64_t SyscallErrorRange = UINT64_MAX - 21;

constexpr bool IsSyscallError(uint64_t value)
{
    return value >= SyscallErrorBase;
}

// Exit statuses the kernel reports for programs it terminated: after a CPU exception, or because
// Ctrl+C interrupted their job.
constexpr uint64_t ExitStatusFault = UINT64_MAX;
constexpr uint64_t ExitStatusKernelAccessFault = UINT64_MAX - 1;
constexpr uint64_t ExitStatusInterrupted = UINT64_MAX - 2;

} // namespace FeatherOS::User

#endif
