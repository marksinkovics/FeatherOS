#ifndef FEATHEROS_SYNC_MUTEX_H
#define FEATHEROS_SYNC_MUTEX_H

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Sync
{

// A lock whose waiters block instead of spinning, for work that runs long, such as disk I/O.
// Only tasks may take it; interrupt handlers must not.
class CAPABILITY("mutex") Mutex
{
public:
    constexpr Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    // The analysis cannot see that held is the lock, so it trusts these two.
    void Lock() ACQUIRE() NO_THREAD_SAFETY_ANALYSIS
    {
        const uint64_t flags = state_lock.LockIrqSave();
        while (held)
        {
            Scheduler::WaitOn(waiters, state_lock);
        }
        held = true;
        state_lock.UnlockIrqRestore(flags);
    }

    void Unlock() RELEASE() NO_THREAD_SAFETY_ANALYSIS
    {
        const uint64_t flags = state_lock.LockIrqSave();
        held = false;
        Scheduler::WakeAll(waiters);
        state_lock.UnlockIrqRestore(flags);
    }

private:
    SpinLock state_lock;
    Scheduler::WaitQueue waiters;
    bool held GUARDED_BY(state_lock) = false;
};

class SCOPED_CAPABILITY MutexGuard
{
public:
    explicit MutexGuard(Mutex& mutex) ACQUIRE(mutex) : mutex(mutex)
    {
        mutex.Lock();
    }
    ~MutexGuard() RELEASE()
    {
        mutex.Unlock();
    }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& mutex;
};

// A pseudo-capability: code that holds no real lock but is known to run under a volume's lock
// says so with it.
class CAPABILITY("role") Role
{
};

// Called when a task takes a volume lock while holding another; the kernel panics, host tests
// count.
void VolumeLockViolation();

// A Mutex that records its owner. A task holds at most one volume lock, except in Quiesce.
class CAPABILITY("mutex") VolumeMutex
{
public:
    constexpr VolumeMutex() = default;
    VolumeMutex(const VolumeMutex&) = delete;
    VolumeMutex& operator=(const VolumeMutex&) = delete;

    void Lock() ACQUIRE() NO_THREAD_SAFETY_ANALYSIS;
    // Takes the lock beside others the task holds, for the one task that takes them all.
    void LockWithOthers() ACQUIRE() NO_THREAD_SAFETY_ANALYSIS;
    void Unlock() RELEASE() NO_THREAD_SAFETY_ANALYSIS;
    bool HeldByCurrentTask() const;

private:
    void Acquire() NO_THREAD_SAFETY_ANALYSIS;

    SpinLock state_lock;
    Scheduler::WaitQueue waiters;
    // Task id plus one, so the boot task (id 0) is an owner; 0 is free. Only the owner can make
    // it its own id, so HeldByCurrentTask reads it without the lock.
    std::atomic<uint64_t> owner = 0;
};

// Takes the volume lock unless the task already holds it, and nothing for a null mutex; the
// analysis is left to the derived hold.
class VolumeHoldBase
{
public:
    explicit VolumeHoldBase(VolumeMutex* mutex) NO_THREAD_SAFETY_ANALYSIS;
    explicit VolumeHoldBase(VolumeMutex& mutex) NO_THREAD_SAFETY_ANALYSIS;
    ~VolumeHoldBase() NO_THREAD_SAFETY_ANALYSIS;
    VolumeHoldBase(const VolumeHoldBase&) = delete;
    VolumeHoldBase& operator=(const VolumeHoldBase&) = delete;

private:
    VolumeMutex* mutex;
    bool locked_here;
};

} // namespace FeatherOS::Sync

#endif
