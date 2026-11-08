#ifndef FEATHEROS_SYNC_MUTEX_H
#define FEATHEROS_SYNC_MUTEX_H

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"

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

} // namespace FeatherOS::Sync

#endif
