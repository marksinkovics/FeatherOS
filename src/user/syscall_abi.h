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
    Spawn = 3,             // (command_line) -> process ID of the started child
    Wait = 4,              // (process_id, status_pointer) -> 0 once the child has exited
    ReadClock = 5,         // () -> current UTC time in seconds since 1970-01-01
    ExtendHeap = 6,        // (increment) -> previous program break; 0 queries it
    Sleep = 7,             // (milliseconds) -> 0 after at least that long
    ReadUptime = 8,        // () -> milliseconds since the kernel's timer started
    CurrentCpu = 9,        // () -> index of the CPU running the caller
    Open = 10,             // (path, flags) -> file descriptor
    Read = 11,             // (descriptor, buffer, length) -> bytes read; 0 at end of file
    Write = 12,            // (descriptor, buffer, length) -> bytes written
    Close = 13,            // (descriptor) -> 0
    Seek = 14,             // (descriptor, offset) -> 0; offset at most the file size
    Remove = 15,           // (path) -> 0
    ReadDirectory = 16,    // (path, index, entry) -> 0; SyscallErrorNotFound past the last entry
    MakeDirectory = 17,    // (path) -> 0
    RemoveDirectory = 18,  // (path) -> 0; the directory must be empty
    ChangeDirectory = 19,  // (path) -> 0
    CurrentDirectory = 20, // (buffer, capacity) -> length of the working directory
};

// Paths are absolute ("/disk/notes.txt") or relative to the process's working directory, which
// starts as its parent's ("/disk" for the shell). "/" is the read-only initramfs; files under
// /disk are writable.
constexpr uint64_t OpenRead = 1U << 0;
constexpr uint64_t OpenWrite = 1U << 1;
// With OpenWrite: create the file if it is missing, or empty it if it exists.
constexpr uint64_t OpenCreate = 1U << 2;
constexpr uint64_t OpenTruncate = 1U << 3;
// With OpenWrite: start at the end of the file.
constexpr uint64_t OpenAppend = 1U << 4;
constexpr uint64_t MaximumPathLength = 64;
// Descriptors 0-2 are reserved for standard streams.
constexpr uint64_t FirstFileDescriptor = 3;
constexpr uint64_t MaximumOpenFiles = 16;

struct DirectoryEntry
{
    // The long name if the file has one, otherwise its 8.3 name, and a terminator.
    char name[64];
    bool directory;
    uint64_t size;
};

// Results at or above SyscallErrorBase report failures instead of values.
constexpr uint64_t SyscallErrorBase = UINT64_MAX - 15;
constexpr uint64_t SyscallErrorFailed = UINT64_MAX;
constexpr uint64_t SyscallErrorNotFound = UINT64_MAX - 1;
constexpr uint64_t SyscallErrorInvalidArgument = UINT64_MAX - 2;
constexpr uint64_t SyscallErrorNotForeground = UINT64_MAX - 3;
constexpr uint64_t SyscallErrorInterrupted = UINT64_MAX - 4;
constexpr uint64_t SyscallErrorTooManyOpenFiles = UINT64_MAX - 5;
// A disk file has either one writer or any number of readers, and an open file cannot be removed.
constexpr uint64_t SyscallErrorBusy = UINT64_MAX - 6;

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
