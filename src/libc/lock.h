#ifndef FEATHEROS_LIBC_LOCK_H
#define FEATHEROS_LIBC_LOCK_H

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

struct Lock
{
    int state = 0;

    void Acquire()
    {
        LockWord(&state, false);
    }

    void Release()
    {
        UnlockWord(&state);
    }
};

// One thread may take it again while holding it, as stdio's flockfile requires.
struct RecursiveLock
{
    int state;
    unsigned long owner;
    unsigned count;

    void Acquire();
    bool TryAcquire();
    void Release();
};

template <typename Held> class Guard
{
public:
    explicit Guard(Held& lock) : m_lock(lock)
    {
        m_lock.Acquire();
    }

    ~Guard()
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
