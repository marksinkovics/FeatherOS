// SPDX-License-Identifier: MIT OR Apache-2.0
// host.h over the C++ standard library, for the tests and command-line tools on macOS, Linux and
// Windows.
#include "standard_host.h"

#include "fs/host.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>

namespace FeatherDisk::Host
{

struct Lock
{
    std::mutex mutex;
};

namespace
{

constexpr std::uint64_t NotFrozen = UINT64_MAX;
std::atomic<std::uint64_t> frozen_clock{NotFrozen};

} // namespace

void* Allocate(std::size_t size)
{
    return std::malloc(size);
}

void Free(void* pointer)
{
    std::free(pointer);
}

Lock* CreateLock()
{
    return new (std::nothrow) Lock;
}

void DestroyLock(Lock* lock)
{
    delete lock;
}

void Acquire(Lock& lock)
{
    lock.mutex.lock();
}

void Release(Lock& lock)
{
    lock.mutex.unlock();
}

void Log(LogLevel level, std::string_view message)
{
    constexpr const char* Names[] = {"debug", "info", "warning", "error"};
    std::fprintf(stderr, "featherdisk %s: %.*s\n", Names[static_cast<std::size_t>(level)],
                 static_cast<int>(message.size()), message.data());
}

void FreezeWallClock(std::optional<std::uint64_t> nanoseconds)
{
    frozen_clock = nanoseconds.value_or(NotFrozen);
}

std::optional<std::uint64_t> WallClockNanoseconds()
{
    if (const std::uint64_t frozen = frozen_clock; frozen != NotFrozen)
    {
        return frozen;
    }
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

} // namespace FeatherDisk::Host
