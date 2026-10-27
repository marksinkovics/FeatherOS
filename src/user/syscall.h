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
inline uint64_t Spawn(const char* command_line, uint64_t flags = 0,
                      const char* environment = nullptr)
{
    return Syscall(SyscallNumber::Spawn, reinterpret_cast<uint64_t>(command_line), flags,
                   reinterpret_cast<uint64_t>(environment));
}

// Waits for a child started with Spawn and stores its exit status.
inline uint64_t Wait(uint64_t process_id, uint64_t* status, uint64_t flags = 0)
{
    return Syscall(SyscallNumber::Wait, process_id, reinterpret_cast<uint64_t>(status), flags);
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

inline uint64_t Sleep(uint64_t milliseconds)
{
    return Syscall(SyscallNumber::Sleep, milliseconds);
}

inline uint64_t ReadUptime()
{
    return Syscall(SyscallNumber::ReadUptime);
}

inline uint64_t CurrentCpu()
{
    return Syscall(SyscallNumber::CurrentCpu);
}

inline uint64_t OpenFile(const char* path, uint64_t flags)
{
    return Syscall(SyscallNumber::Open, reinterpret_cast<uint64_t>(path), flags);
}

inline uint64_t ReadFile(uint64_t descriptor, void* buffer, uint64_t length)
{
    return Syscall(SyscallNumber::Read, descriptor, reinterpret_cast<uint64_t>(buffer), length);
}

inline uint64_t WriteFile(uint64_t descriptor, const void* buffer, uint64_t length)
{
    return Syscall(SyscallNumber::Write, descriptor, reinterpret_cast<uint64_t>(buffer), length);
}

inline uint64_t CloseFile(uint64_t descriptor)
{
    return Syscall(SyscallNumber::Close, descriptor);
}

inline uint64_t SeekFile(uint64_t descriptor, uint64_t offset, uint64_t whence = SeekFromStart)
{
    return Syscall(SyscallNumber::Seek, descriptor, offset, whence);
}

inline uint64_t RemoveFile(const char* path)
{
    return Syscall(SyscallNumber::Remove, reinterpret_cast<uint64_t>(path));
}

inline uint64_t MakeDirectory(const char* path)
{
    return Syscall(SyscallNumber::MakeDirectory, reinterpret_cast<uint64_t>(path));
}

inline uint64_t RemoveDirectory(const char* path)
{
    return Syscall(SyscallNumber::RemoveDirectory, reinterpret_cast<uint64_t>(path));
}

inline uint64_t Rename(const char* from, const char* to)
{
    return Syscall(SyscallNumber::Rename, reinterpret_cast<uint64_t>(from),
                   reinterpret_cast<uint64_t>(to));
}

inline uint64_t Shutdown(uint64_t action)
{
    return Syscall(SyscallNumber::Shutdown, action);
}

inline uint64_t ChangeDirectory(const char* path)
{
    return Syscall(SyscallNumber::ChangeDirectory, reinterpret_cast<uint64_t>(path));
}

inline uint64_t CurrentDirectory(char* buffer, uint64_t capacity)
{
    return Syscall(SyscallNumber::CurrentDirectory, reinterpret_cast<uint64_t>(buffer), capacity);
}

inline uint64_t ReadDirectory(const char* path, uint64_t index, DirectoryEntry* entry)
{
    return Syscall(SyscallNumber::ReadDirectory, reinterpret_cast<uint64_t>(path), index,
                   reinterpret_cast<uint64_t>(entry));
}

inline uint64_t Stat(const char* path, FileStatus* status)
{
    return Syscall(SyscallNumber::Stat, reinterpret_cast<uint64_t>(path),
                   reinterpret_cast<uint64_t>(status));
}

inline uint64_t StatDescriptor(uint64_t descriptor, FileStatus* status)
{
    return Syscall(SyscallNumber::StatDescriptor, descriptor, reinterpret_cast<uint64_t>(status));
}

inline uint64_t Duplicate(uint64_t descriptor, uint64_t target, uint64_t flags)
{
    return Syscall(SyscallNumber::Duplicate, descriptor, target, flags);
}

inline uint64_t DescriptorFlags(uint64_t descriptor, bool set, uint64_t flags)
{
    return Syscall(SyscallNumber::DescriptorFlags, descriptor, set ? 1 : 0, flags);
}

inline uint64_t CreatePipe(uint64_t (&descriptors)[2], uint64_t flags)
{
    return Syscall(SyscallNumber::Pipe, reinterpret_cast<uint64_t>(descriptors), flags);
}

inline uint64_t ProcessGroup(uint64_t process_id)
{
    return Syscall(SyscallNumber::ProcessGroup, process_id);
}

inline uint64_t ForegroundGroup(bool set, uint64_t group)
{
    return Syscall(SyscallNumber::ForegroundGroup, set ? 1 : 0, group);
}

inline uint64_t ProcessId()
{
    return Syscall(SyscallNumber::ProcessId);
}

inline uint64_t ParentProcessId()
{
    return Syscall(SyscallNumber::ParentProcessId);
}

} // namespace FeatherOS::User

#endif
