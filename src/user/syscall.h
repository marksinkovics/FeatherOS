#ifndef FEATHEROS_USER_SYSCALL_H
#define FEATHEROS_USER_SYSCALL_H

#include "syscall_abi.h"

#include <stdint.h>

namespace FeatherOS::User
{

inline uint64_t Syscall(SyscallNumber number, uint64_t first = 0, uint64_t second = 0,
                        uint64_t third = 0)
{
#if defined(__x86_64__)
    uint64_t result = static_cast<uint64_t>(number);
    asm volatile("int $0x80" : "+a"(result) : "D"(first), "S"(second), "d"(third) : "memory", "cc");
    return result;
#elif defined(__aarch64__)
    register uint64_t syscall_number asm("x8") = static_cast<uint64_t>(number);
    register uint64_t result asm("x0") = first;
    register uint64_t argument1 asm("x1") = second;
    register uint64_t argument2 asm("x2") = third;
    asm volatile("svc #0"
                 : "+r"(result)
                 : "r"(syscall_number), "r"(argument1), "r"(argument2)
                 : "memory", "cc");
    return result;
#else
#error "Unsupported userspace syscall architecture"
#endif
}

inline uint64_t WriteCharacter(uint64_t character)
{
    return Syscall(SyscallNumber::WriteCharacter, character);
}

[[noreturn]] inline void Exit(uint64_t status)
{
    Syscall(SyscallNumber::Exit, status);
    __builtin_trap();
}

// Blocks until the user presses Enter; returns the line length or a syscall error.
inline uint64_t ReadLine(char* buffer, uint64_t capacity, const char* prompt)
{
    return Syscall(SyscallNumber::ReadLine, reinterpret_cast<uint64_t>(buffer), capacity,
                   reinterpret_cast<uint64_t>(prompt));
}

// Starts the program named by the first word of command_line; returns its process ID.
inline uint64_t Spawn(const char* command_line)
{
    return Syscall(SyscallNumber::Spawn, reinterpret_cast<uint64_t>(command_line));
}

// Waits for a child started with Spawn and stores its exit status.
inline uint64_t Wait(uint64_t process_id, uint64_t* status)
{
    return Syscall(SyscallNumber::Wait, process_id, reinterpret_cast<uint64_t>(status));
}

// Returns the current UTC time in seconds since 1970-01-01, or a syscall error.
inline uint64_t ReadClock()
{
    return Syscall(SyscallNumber::ReadClock);
}

inline uint64_t ExtendHeap(uint64_t increment)
{
    return Syscall(SyscallNumber::ExtendHeap, increment);
}

} // namespace FeatherOS::User

#endif
