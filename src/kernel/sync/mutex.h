#ifndef FEATHEROS_SYNC_MUTEX_H
#define FEATHEROS_SYNC_MUTEX_H

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Sync
{

// A lock whose waiters block instead of spinning, for work that runs long, such as disk I/O.
// Only tasks may take it; interrupt handlers must not.
class Mutex
{
public:
    constexpr Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock()
    {
        const uint64_t flags = state_lock.LockIrqSave();
        while (held)
        {
            Scheduler::WaitOn(waiters, state_lock);
        }
        held = true;
        state_lock.UnlockIrqRestore(flags);
    }

    void Unlock()
    {
        const uint64_t flags = state_lock.LockIrqSave();
        held = false;
        Scheduler::WakeAll(waiters);
        state_lock.UnlockIrqRestore(flags);
    }

private:
    SpinLock state_lock;
    Scheduler::WaitQueue waiters;
    bool held = false;
};

class MutexGuard
{
public:
    explicit MutexGuard(Mutex& mutex) : mutex(mutex)
    {
        mutex.Lock();
    }
    ~MutexGuard()
    {
        mutex.Unlock();
    }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& mutex;
};

} // namespace FeatherOS::Sync

#endif
