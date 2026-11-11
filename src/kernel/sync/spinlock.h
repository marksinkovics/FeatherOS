#ifndef FEATHEROS_SYNC_SPINLOCK_H
#define FEATHEROS_SYNC_SPINLOCK_H

#include "kernel/arch/interrupts.h"
#include "user/thread_safety.h"

#include <atomic>
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
        while (locked.exchange(1U, std::memory_order_acquire) != 0)
        {
            while (locked.load(std::memory_order_relaxed) != 0)
            {
                CpuRelax();
            }
        }
    }

    bool TryLock() TRY_ACQUIRE(true) NO_THREAD_SAFETY_ANALYSIS
    {
        return locked.exchange(1U, std::memory_order_acquire) == 0;
    }

    void Unlock() RELEASE() NO_THREAD_SAFETY_ANALYSIS
    {
        locked.store(0U, std::memory_order_release);
    }

    bool IsLocked() const
    {
        return locked.load(std::memory_order_relaxed) != 0;
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
    std::atomic<uint32_t> locked = 0;
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
