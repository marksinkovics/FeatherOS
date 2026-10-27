#include "lock.h"

#include "user/syscall.h"

namespace FeatherOS::LibC
{
namespace
{

// The template's 1 is the main thread's ID; new threads set their own when they start.
__thread unsigned long current_thread = 1;

} // namespace

uint64_t FutexWait(int* word, int expected, uint64_t milliseconds)
{
    return User::Syscall(User::SyscallNumber::FutexWait, reinterpret_cast<uint64_t>(word),
                         static_cast<uint32_t>(expected), milliseconds);
}

void FutexWake(int* word, uint64_t count)
{
    User::Syscall(User::SyscallNumber::FutexWake, reinterpret_cast<uint64_t>(word), count);
}

unsigned long CurrentThread()
{
    return current_thread;
}

void SetCurrentThread(unsigned long thread)
{
    current_thread = thread;
}

void LockWord(int* state, bool contended_from_start)
{
    int current = 0;
    if (!contended_from_start &&
        __atomic_compare_exchange_n(state, &current, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        return;
    }
    current = __atomic_exchange_n(state, 2, __ATOMIC_ACQUIRE);
    while (current != 0)
    {
        FutexWait(state, 2);
        current = __atomic_exchange_n(state, 2, __ATOMIC_ACQUIRE);
    }
}

bool TryLockWord(int* state)
{
    int unlocked = 0;
    return __atomic_compare_exchange_n(state, &unlocked, 1, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

void UnlockWord(int* state)
{
    if (__atomic_exchange_n(state, 0, __ATOMIC_RELEASE) == 2)
    {
        FutexWake(state, 1);
    }
}

void RecursiveLock::Acquire()
{
    if (__atomic_load_n(&owner, __ATOMIC_RELAXED) == current_thread)
    {
        ++count;
        return;
    }
    LockWord(&state, false);
    __atomic_store_n(&owner, current_thread, __ATOMIC_RELAXED);
    count = 1;
}

bool RecursiveLock::TryAcquire()
{
    if (__atomic_load_n(&owner, __ATOMIC_RELAXED) == current_thread)
    {
        ++count;
        return true;
    }
    if (!TryLockWord(&state))
    {
        return false;
    }
    __atomic_store_n(&owner, current_thread, __ATOMIC_RELAXED);
    count = 1;
    return true;
}

void RecursiveLock::Release()
{
    if (--count > 0)
    {
        return;
    }
    __atomic_store_n(&owner, 0, __ATOMIC_RELAXED);
    UnlockWord(&state);
}

} // namespace FeatherOS::LibC
