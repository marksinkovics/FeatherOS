// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_HOST_H
#define FEATHERDISK_HOST_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Declared by the core and defined once by each host (a kernel, an app, a command-line tool),
// which links its definitions beside the core library.
namespace FeatherDisk::Host
{

// Aligned for any object of up to 16 bytes; null when memory runs out.
void* Allocate(std::size_t size);
// Takes null, as free does.
void Free(void* pointer);

// Defined by the host. The core holds a lock only briefly and never across a BlockDevice call,
// so a kernel may spin and an app may use a mutex. Locks are not recursive.
struct Lock;
Lock* CreateLock();
void DestroyLock(Lock* lock);
void Acquire(Lock& lock);
void Release(Lock& lock);

enum class LogLevel : std::uint8_t
{
    Debug,
    Info,
    Warning,
    Error,
};

// The message is one line without a newline, and is not null-terminated.
void Log(LogLevel level, std::string_view message);

// UTC since 1970-01-01, for timestamps on disk; nullopt when the host does not know the time.
std::optional<std::uint64_t> WallClockNanoseconds();

} // namespace FeatherDisk::Host

#endif
