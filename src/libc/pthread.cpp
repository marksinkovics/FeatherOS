#include "syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

namespace
{

// The main thread counts too, so the last thread to call pthread_exit knows to end the process.
int live_threads = 1;
// Each thread's ID, so pthread_self needs no syscall. The template's 1 is the main thread's.
__thread pthread_t current_thread = 1;

// Sleeps while *word holds expected, for at most milliseconds (0 waits forever).
uint64_t FutexWait(int* word, int expected, uint64_t milliseconds = 0)
{
    return FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::FutexWait,
                                    reinterpret_cast<uint64_t>(word),
                                    static_cast<uint32_t>(expected), milliseconds);
}

void FutexWake(int* word, uint64_t count)
{
    FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::FutexWake,
                             reinterpret_cast<uint64_t>(word), count);
}

// Takes the lock word, marking it contended (2) whenever it had to wait, so the unlock that
// follows wakes the next waiter.
void LockWord(int* state, int contended_from_start)
{
    int current = 0;
    if (contended_from_start == 0 &&
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

void UnlockWord(int* state)
{
    if (__atomic_exchange_n(state, 0, __ATOMIC_RELEASE) == 2)
    {
        FutexWake(state, 1);
    }
}

// The owner checks ERRORCHECK and RECURSIVE mutexes make; 0 lets the caller take the lock.
int CheckOwnerLock(pthread_mutex_t* mutex)
{
    if (mutex->type == PTHREAD_MUTEX_NORMAL ||
        __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != current_thread)
    {
        return 0;
    }
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK)
    {
        return EDEADLK;
    }
    ++mutex->count;
    return -1;
}

void TakeOwnership(pthread_mutex_t* mutex)
{
    __atomic_store_n(&mutex->owner, current_thread, __ATOMIC_RELAXED);
    mutex->count = 1;
}

int LockMutex(pthread_mutex_t* mutex, int contended)
{
    const int owned = CheckOwnerLock(mutex);
    if (owned != 0)
    {
        return owned < 0 ? 0 : owned;
    }
    LockWord(&mutex->state, contended);
    TakeOwnership(mutex);
    return 0;
}

uint64_t MillisecondsUntil(clockid_t clock, const struct timespec* deadline, bool* passed)
{
    struct timespec now = {};
    clock_gettime(clock, &now);
    const long long seconds = static_cast<long long>(deadline->tv_sec - now.tv_sec);
    const long long nanoseconds = deadline->tv_nsec - now.tv_nsec;
    const long long total = seconds * 1000000000LL + nanoseconds;
    *passed = total <= 0;
    // Rounded up, so a wait never ends before the deadline.
    return total <= 0 ? 0 : static_cast<uint64_t>((total + 999999) / 1000000);
}

[[noreturn]] void EndThread(void* value)
{
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL) == 0)
    {
        exit(0);
    }
    FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadExit,
                             reinterpret_cast<uint64_t>(value));
    __builtin_unreachable();
}

// Every new thread starts here, with block pointing to {start, argument} at its stack's top.
extern "C" [[noreturn]] void FeatherOSThreadStart(const uint64_t* block)
{
    current_thread = FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadId);
    auto* start = reinterpret_cast<void* (*)(void*)>(block[0]);
    EndThread(start(reinterpret_cast<void*>(block[1])));
}

} // namespace

int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                   void* (*start)(void* argument), void* argument)
{
    if (thread == nullptr || start == nullptr)
    {
        return EINVAL;
    }
    __atomic_add_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
    const uint64_t result = FeatherOS::User::Syscall(
        FeatherOS::User::SyscallNumber::ThreadCreate,
        reinterpret_cast<uint64_t>(&FeatherOSThreadStart), reinterpret_cast<uint64_t>(start),
        reinterpret_cast<uint64_t>(argument));
    if (FeatherOS::User::IsSyscallError(result))
    {
        __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
        return result == FeatherOS::User::SyscallErrorWouldBlock
                   ? EAGAIN
                   : FeatherOS::LibC::ErrorNumber(result);
    }
    *thread = result;
    if (attributes != nullptr && attributes->detach_state == PTHREAD_CREATE_DETACHED)
    {
        pthread_detach(result);
    }
    return 0;
}

int pthread_join(pthread_t thread, void** value)
{
    if (thread == pthread_self())
    {
        return EDEADLK;
    }
    uint64_t result = 0;
    const uint64_t status = FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadJoin,
                                                     thread, reinterpret_cast<uint64_t>(&result));
    if (status == FeatherOS::User::SyscallErrorNotFound)
    {
        return ESRCH;
    }
    if (status == FeatherOS::User::SyscallErrorInvalidArgument)
    {
        return EINVAL;
    }
    if (FeatherOS::User::IsSyscallError(status))
    {
        return FeatherOS::LibC::ErrorNumber(status);
    }
    if (value != nullptr)
    {
        *value = reinterpret_cast<void*>(result);
    }
    return 0;
}

void pthread_exit(void* value)
{
    EndThread(value);
}

pthread_t pthread_self(void)
{
    return current_thread;
}

int pthread_detach(pthread_t thread)
{
    const uint64_t status =
        FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadDetach, thread);
    if (status == FeatherOS::User::SyscallErrorNotFound)
    {
        return ESRCH;
    }
    return FeatherOS::User::IsSyscallError(status) ? EINVAL : 0;
}

int pthread_equal(pthread_t first, pthread_t second)
{
    return first == second ? 1 : 0;
}

int pthread_attr_init(pthread_attr_t* attributes)
{
    attributes->detach_state = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t*)
{
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attributes, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
    {
        return EINVAL;
    }
    attributes->detach_state = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attributes, int* state)
{
    *state = attributes->detach_state;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attributes)
{
    mutex->state = 0;
    mutex->type = attributes != nullptr ? attributes->type : PTHREAD_MUTEX_DEFAULT;
    mutex->owner = 0;
    mutex->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
    return __atomic_load_n(&mutex->state, __ATOMIC_RELAXED) != 0 ? EBUSY : 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex)
{
    return LockMutex(mutex, 0);
}

int pthread_mutex_trylock(pthread_mutex_t* mutex)
{
    const int owned = CheckOwnerLock(mutex);
    if (owned != 0)
    {
        return owned < 0 ? 0 : EBUSY;
    }
    int unlocked = 0;
    if (!__atomic_compare_exchange_n(&mutex->state, &unlocked, 1, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
    {
        return EBUSY;
    }
    TakeOwnership(mutex);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
    if (mutex->type != PTHREAD_MUTEX_NORMAL)
    {
        if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != current_thread)
        {
            return EPERM;
        }
        if (--mutex->count > 0)
        {
            return 0;
        }
    }
    __atomic_store_n(&mutex->owner, 0, __ATOMIC_RELAXED);
    UnlockWord(&mutex->state);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t* attributes)
{
    attributes->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t*)
{
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attributes, int type)
{
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_ERRORCHECK &&
        type != PTHREAD_MUTEX_RECURSIVE)
    {
        return EINVAL;
    }
    attributes->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* attributes, int* type)
{
    *type = attributes->type;
    return 0;
}

int pthread_cond_init(pthread_cond_t* condition, const pthread_condattr_t* attributes)
{
    condition->sequence = 0;
    condition->clock = attributes != nullptr ? attributes->clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t*)
{
    return 0;
}

int pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex)
{
    return pthread_cond_timedwait(condition, mutex, nullptr);
}

// The sequence number is read before the mutex is released, so a signal after the unlock
// changes it and the wait returns at once instead of missing it.
int pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                           const struct timespec* deadline)
{
    if (deadline != nullptr && (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000))
    {
        return EINVAL;
    }
    int* sequence = reinterpret_cast<int*>(&condition->sequence);
    const int seen = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    const unsigned count = mutex->count;
    mutex->count = 1;
    pthread_mutex_unlock(mutex);
    uint64_t result = 0;
    if (deadline == nullptr)
    {
        result = FutexWait(sequence, seen);
    }
    else
    {
        bool passed = false;
        const uint64_t milliseconds = MillisecondsUntil(condition->clock, deadline, &passed);
        result = passed ? FeatherOS::User::SyscallErrorTimedOut
                        : FutexWait(sequence, seen, milliseconds);
    }
    // Another thread may be waiting by now, so the mutex is taken as contended.
    LockMutex(mutex, 1);
    mutex->count = count;
    return result == FeatherOS::User::SyscallErrorTimedOut ? ETIMEDOUT : 0;
}

int pthread_cond_signal(pthread_cond_t* condition)
{
    int* sequence = reinterpret_cast<int*>(&condition->sequence);
    __atomic_add_fetch(sequence, 1, __ATOMIC_RELEASE);
    FutexWake(sequence, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* condition)
{
    int* sequence = reinterpret_cast<int*>(&condition->sequence);
    __atomic_add_fetch(sequence, 1, __ATOMIC_RELEASE);
    FutexWake(sequence, INT_MAX);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t* attributes)
{
    attributes->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t*)
{
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t* attributes, clockid_t clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
    {
        return EINVAL;
    }
    attributes->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t* attributes, clockid_t* clock)
{
    *clock = attributes->clock;
    return 0;
}

// 0 not run yet, 1 running, 2 done; latecomers sleep until the first caller finishes.
int pthread_once(pthread_once_t* once, void (*routine)(void))
{
    int state = 0;
    if (__atomic_compare_exchange_n(once, &state, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
    {
        routine();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        FutexWake(once, INT_MAX);
        return 0;
    }
    while (state == 1)
    {
        FutexWait(once, 1);
        state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
    }
    return 0;
}
