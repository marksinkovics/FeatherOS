#ifndef FEATHEROS_SYNC_SPINLOCK_H
#define FEATHEROS_SYNC_SPINLOCK_H

#include "kernel/arch/interrupts.h"
#include "user/thread_safety.h"

#include <stdint.h>

namespace FeatherOS::Sync
{

inline void CpuRelax()
{
#if defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// The primitives below are what the thread-safety analysis trusts, so it does not check them.
class CAPABILITY("spinlock") SpinLock
{
public:
    // constexpr: global locks are usable before global constructors run.
    constexpr SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void Lock() ACQUIRE() NO_THREAD_SAFETY_ANALYSIS
    {
        while (__atomic_exchange_n(&locked, 1U, __ATOMIC_ACQUIRE) != 0)
        {
            while (__atomic_load_n(&locked, __ATOMIC_RELAXED) != 0)
            {
                CpuRelax();
            }
        }
    }

    bool TryLock() TRY_ACQUIRE(true) NO_THREAD_SAFETY_ANALYSIS
    {
        return __atomic_exchange_n(&locked, 1U, __ATOMIC_ACQUIRE) == 0;
    }

    void Unlock() RELEASE() NO_THREAD_SAFETY_ANALYSIS
    {
        __atomic_store_n(&locked, 0U, __ATOMIC_RELEASE);
    }

    bool IsLocked() const
    {
        return __atomic_load_n(&locked, __ATOMIC_RELAXED) != 0;
    }

    uint64_t LockIrqSave() ACQUIRE()
    {
        const uint64_t flags = Arch::Interrupts::SaveAndDisable();
        Lock();
        return flags;
    }

    void UnlockIrqRestore(uint64_t flags) RELEASE()
    {
        Unlock();
        Arch::Interrupts::Restore(flags);
    }

private:
    uint32_t locked = 0;
};

class SCOPED_CAPABILITY SpinLockGuard
{
public:
    explicit SpinLockGuard(SpinLock& lock) ACQUIRE(lock) : lock(lock), flags(lock.LockIrqSave()) {}
    ~SpinLockGuard() RELEASE()
    {
        lock.UnlockIrqRestore(flags);
    }
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& lock;
    uint64_t flags;
};

} // namespace FeatherOS::Sync

#endif
