#ifndef FEATHEROS_LIBC_LOCK_H
#define FEATHEROS_LIBC_LOCK_H

#include "user/thread_safety.h"

#include <stdint.h>

// The C library's own locks, which sleep in the kernel (FutexWait) only while contended. Both
// are plain structures that start unlocked as zero, so static ones need no constructor.
namespace FeatherOS::LibC
{

// Sleeps while *word holds expected, for at most milliseconds (0 waits forever); returns the
// syscall's result.
uint64_t FutexWait(int* word, int expected, uint64_t milliseconds = 0);
void FutexWake(int* word, uint64_t count);

// The calling thread's ID, kept in TLS so it needs no syscall.
unsigned long CurrentThread();
void SetCurrentThread(unsigned long thread);

// A lock word: 0 unlocked, 1 locked, 2 locked with threads that may be waiting.
// contended_from_start marks it 2 right away, for a thread that a waiter may already follow.
void LockWord(int* state, bool contended_from_start);
bool TryLockWord(int* state);
void UnlockWord(int* state);

// The analysis trusts these primitives without checking them.
struct CAPABILITY("mutex") Lock
{
    int state = 0;

    void Acquire() ACQUIRE() NO_THREAD_SAFETY_ANALYSIS
    {
        LockWord(&state, false);
    }

    void Release() RELEASE() NO_THREAD_SAFETY_ANALYSIS
    {
        UnlockWord(&state);
    }
};

// One thread may take it again while holding it, as stdio's flockfile requires.
struct CAPABILITY("mutex") RecursiveLock
{
    int state;
    unsigned long owner;
    unsigned count;

    void Acquire() ACQUIRE() NO_THREAD_SAFETY_ANALYSIS;
    bool TryAcquire() TRY_ACQUIRE(true) NO_THREAD_SAFETY_ANALYSIS;
    void Release() RELEASE() NO_THREAD_SAFETY_ANALYSIS;
};

template <typename Held> class SCOPED_CAPABILITY Guard
{
public:
    explicit Guard(Held& lock) ACQUIRE(lock) : m_lock(lock)
    {
        m_lock.Acquire();
    }

    ~Guard() RELEASE()
    {
        m_lock.Release();
    }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    Held& m_lock;
};

} // namespace FeatherOS::LibC

#endif
