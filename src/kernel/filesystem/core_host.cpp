// host.h for the shared filesystem core (src/fs/core/), over the kernel heap, spinlocks, KLogger
// and the wall clock.
#include "fs/host.h"

#include "kernel/klogger.h"
#include "kernel/memory/heap.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/wall_clock.h"
#include "user/thread_safety.h"

#include <new>

namespace FeatherDisk::Host
{

struct Lock
{
    FeatherOS::Sync::SpinLock spin;
    uint64_t interrupt_flags GUARDED_BY(spin) = 0;
};

namespace
{

// Without a terminal attached it writes to the serial port only.
constinit FeatherOS::KLogger logger;

} // namespace

void* Allocate(std::size_t size)
{
    return FeatherOS::Memory::Allocate(size);
}

void Free(void* pointer)
{
    if (pointer != nullptr)
    {
        FeatherOS::Memory::Deallocate(pointer);
    }
}

Lock* CreateLock()
{
    void* memory = FeatherOS::Memory::Allocate(sizeof(Lock));
    return memory == nullptr ? nullptr : new (memory) Lock;
}

void DestroyLock(Lock* lock)
{
    if (lock != nullptr)
    {
        lock->~Lock();
        FeatherOS::Memory::Deallocate(lock);
    }
}

// The analysis cannot follow a lock taken here and released in another call, so it trusts these
// two, as it trusts SpinLock itself.
void Acquire(Lock& lock) NO_THREAD_SAFETY_ANALYSIS
{
    const uint64_t flags = lock.spin.LockIrqSave();
    lock.interrupt_flags = flags;
}

void Release(Lock& lock) NO_THREAD_SAFETY_ANALYSIS
{
    lock.spin.UnlockIrqRestore(lock.interrupt_flags);
}

void Log(LogLevel level, std::string_view message)
{
    char line[200];
    const size_t length = message.size() < sizeof(line) ? message.size() : sizeof(line) - 1;
    for (size_t index = 0; index < length; ++index)
    {
        line[index] = message[index];
    }
    line[length] = '\0';
    switch (level)
    {
    case LogLevel::Debug:
        logger.Debug(line);
        break;
    case LogLevel::Info:
        logger.Info(line);
        break;
    case LogLevel::Warning:
        logger.Warning(line);
        break;
    case LogLevel::Error:
        logger.Error(line);
        break;
    }
}

std::optional<std::uint64_t> WallClockNanoseconds()
{
    uint64_t nanoseconds = 0;
    if (!FeatherOS::Time::ReadWallClock(&nanoseconds))
    {
        return std::nullopt;
    }
    return nanoseconds;
}

} // namespace FeatherDisk::Host
