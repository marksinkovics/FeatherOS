#ifndef FEATHEROS_USER_SYSCALL_ABI_H
#define FEATHEROS_USER_SYSCALL_ABI_H

#include <stdint.h>

// Syscall numbers and result codes shared by the kernel and user programs.
// x86-64 passes the number in rax and arguments in rdi, rsi, rdx; AArch64 uses x8 and x0-x2.
namespace FeatherOS::User
{

enum class SyscallNumber : uint64_t
{
    Exit = 0,           // (status) -> does not return
    WriteCharacter = 1, // (character) -> 0; printable ASCII or '\n'
    ReadLine = 2,       // (buffer, capacity, prompt) -> line length; foreground process only
    Spawn = 3,          // (command_line) -> process ID of the started child
    Wait = 4,           // (process_id, status_pointer) -> 0 once the child has exited
    ReadClock = 5,      // () -> current UTC time in seconds since 1970-01-01
    ExtendHeap = 6,     // (increment) -> previous program break; 0 queries it
    Sleep = 7,          // (milliseconds) -> 0 after at least that long
    ReadUptime = 8,     // () -> milliseconds since the kernel's timer started
    CurrentCpu = 9,     // () -> index of the CPU running the caller
};

// Results at or above SyscallErrorBase report failures instead of values.
constexpr uint64_t SyscallErrorBase = UINT64_MAX - 15;
constexpr uint64_t SyscallErrorFailed = UINT64_MAX;
constexpr uint64_t SyscallErrorNotFound = UINT64_MAX - 1;
constexpr uint64_t SyscallErrorInvalidArgument = UINT64_MAX - 2;
constexpr uint64_t SyscallErrorNotForeground = UINT64_MAX - 3;
constexpr uint64_t SyscallErrorInterrupted = UINT64_MAX - 4;

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
