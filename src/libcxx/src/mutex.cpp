#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/mutex"
#include "libcxx/include/shared_mutex"
#else
#include <mutex>
#include <shared_mutex>
#endif

#include <pthread.h>

// The mutexes' own members do not follow their annotations, which describe them to callers.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace
{

class InternalLock
{
public:
    explicit InternalLock(pthread_mutex_t* mutex) noexcept : m_mutex(mutex)
    {
        pthread_mutex_lock(m_mutex);
    }
    ~InternalLock()
    {
        pthread_mutex_unlock(m_mutex);
    }
    InternalLock(const InternalLock&) = delete;
    InternalLock& operator=(const InternalLock&) = delete;

private:
    pthread_mutex_t* m_mutex;
};

pthread_mutex_t once_mutex = PTHREAD_MUTEX_INITIALIZER;

// A function-local static, so a call_once in another file's static initializer finds it built.
__detail::Condition& OnceFinished()
{
    static __detail::Condition finished;
    return finished;
}

} // namespace

void mutex::lock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    if (pthread_mutex_lock(&m_handle) != 0)
    {
        __detail::LibraryAbort("mutex::lock failed");
    }
}

bool mutex::try_lock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    return pthread_mutex_trylock(&m_handle) == 0;
}

void mutex::unlock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    pthread_mutex_unlock(&m_handle);
}

recursive_mutex::recursive_mutex()
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m_handle, &attributes);
    pthread_mutexattr_destroy(&attributes);
}

recursive_mutex::~recursive_mutex()
{
    pthread_mutex_destroy(&m_handle);
}

void recursive_mutex::lock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    if (pthread_mutex_lock(&m_handle) != 0)
    {
        __detail::LibraryAbort("recursive_mutex::lock failed");
    }
}

bool recursive_mutex::try_lock() noexcept FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    return pthread_mutex_trylock(&m_handle) == 0;
}

void recursive_mutex::unlock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    pthread_mutex_unlock(&m_handle);
}

void timed_mutex::lock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const InternalLock guard(&m_mutex);
    while (m_locked)
    {
        m_released.Wait(&m_mutex);
    }
    m_locked = true;
}

bool timed_mutex::try_lock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const InternalLock guard(&m_mutex);
    if (m_locked)
    {
        return false;
    }
    m_locked = true;
    return true;
}

bool timed_mutex::TryLockUntil(chrono::steady_clock::time_point deadline)
    FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const InternalLock guard(&m_mutex);
    while (m_locked)
    {
        if (!m_released.WaitUntil(&m_mutex, deadline) && m_locked)
        {
            return false;
        }
    }
    m_locked = true;
    return true;
}

// Notifying before the internal unlock: once it is unlocked, another thread may take, release and
// destroy the mutex.
void timed_mutex::unlock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const InternalLock guard(&m_mutex);
    m_locked = false;
    m_released.NotifyOne();
}

void recursive_timed_mutex::lock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const pthread_t self = pthread_self();
    const InternalLock guard(&m_mutex);
    if (m_count != 0 && pthread_equal(m_owner, self) != 0)
    {
        ++m_count;
        return;
    }
    while (m_count != 0)
    {
        m_released.Wait(&m_mutex);
    }
    m_owner = self;
    m_count = 1;
}

bool recursive_timed_mutex::try_lock() noexcept FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const pthread_t self = pthread_self();
    const InternalLock guard(&m_mutex);
    if (m_count != 0 && pthread_equal(m_owner, self) == 0)
    {
        return false;
    }
    m_owner = self;
    ++m_count;
    return true;
}

bool recursive_timed_mutex::TryLockUntil(chrono::steady_clock::time_point deadline)
    FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const pthread_t self = pthread_self();
    const InternalLock guard(&m_mutex);
    if (m_count != 0 && pthread_equal(m_owner, self) != 0)
    {
        ++m_count;
        return true;
    }
    while (m_count != 0)
    {
        if (!m_released.WaitUntil(&m_mutex, deadline) && m_count != 0)
        {
            return false;
        }
    }
    m_owner = self;
    m_count = 1;
    return true;
}

void recursive_timed_mutex::unlock() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
{
    const InternalLock guard(&m_mutex);
    if (--m_count == 0)
    {
        m_released.NotifyOne();
    }
}

namespace __detail
{

int TryLockAll(ErasedLockable* lockables, int count, int first, bool lock_first) noexcept
{
    for (int step = lock_first ? 1 : 0; step < count; ++step)
    {
        ErasedLockable& next = lockables[(first + step) % count];
        if (!next.m_try_lock(next.m_object))
        {
            for (int taken = step - 1; taken >= 0; --taken)
            {
                ErasedLockable& held = lockables[(first + taken) % count];
                held.m_unlock(held.m_object);
            }
            return (first + step) % count;
        }
    }
    return -1;
}

bool OnceBegin(atomic<int>& state) noexcept
{
    int expected = OnceNotRun;
    if (state.compare_exchange_strong(expected, OnceRunning, memory_order_acquire))
    {
        return true;
    }
    if (expected == OnceDone)
    {
        return false;
    }
    const InternalLock guard(&once_mutex);
    while (state.load(memory_order_acquire) != OnceDone)
    {
        OnceFinished().Wait(&once_mutex);
    }
    return false;
}

void OnceEnd(atomic<int>& state) noexcept
{
    const InternalLock guard(&once_mutex);
    state.store(OnceDone, memory_order_release);
    OnceFinished().NotifyAll();
}

void SharedMutexBase::Lock() noexcept
{
    const InternalLock guard(&m_mutex);
    while ((m_state & WriterEntered) != 0)
    {
        m_gate.Wait(&m_mutex);
    }
    m_state |= WriterEntered;
    while ((m_state & MaxReaders) != 0)
    {
        m_readers_left.Wait(&m_mutex);
    }
}

bool SharedMutexBase::TryLock() noexcept
{
    const InternalLock guard(&m_mutex);
    if (m_state != 0)
    {
        return false;
    }
    m_state = WriterEntered;
    return true;
}

bool SharedMutexBase::TryLockUntil(chrono::steady_clock::time_point deadline) noexcept
{
    const InternalLock guard(&m_mutex);
    while ((m_state & WriterEntered) != 0)
    {
        if (!m_gate.WaitUntil(&m_mutex, deadline) && (m_state & WriterEntered) != 0)
        {
            return false;
        }
    }
    m_state |= WriterEntered;
    while ((m_state & MaxReaders) != 0)
    {
        if (!m_readers_left.WaitUntil(&m_mutex, deadline) && (m_state & MaxReaders) != 0)
        {
            // Reopens the gate for the readers and writers that queued behind this writer.
            m_state &= ~WriterEntered;
            m_gate.NotifyAll();
            return false;
        }
    }
    return true;
}

void SharedMutexBase::Unlock() noexcept
{
    const InternalLock guard(&m_mutex);
    m_state = 0;
    m_gate.NotifyAll();
}

void SharedMutexBase::LockShared() noexcept
{
    const InternalLock guard(&m_mutex);
    while (m_state >= MaxReaders)
    {
        m_gate.Wait(&m_mutex);
    }
    ++m_state;
}

bool SharedMutexBase::TryLockShared() noexcept
{
    const InternalLock guard(&m_mutex);
    if (m_state >= MaxReaders)
    {
        return false;
    }
    ++m_state;
    return true;
}

bool SharedMutexBase::TryLockSharedUntil(chrono::steady_clock::time_point deadline) noexcept
{
    const InternalLock guard(&m_mutex);
    while (m_state >= MaxReaders)
    {
        if (!m_gate.WaitUntil(&m_mutex, deadline) && m_state >= MaxReaders)
        {
            return false;
        }
    }
    ++m_state;
    return true;
}

void SharedMutexBase::UnlockShared() noexcept
{
    const InternalLock guard(&m_mutex);
    const unsigned previous = m_state--;
    if ((m_state & WriterEntered) != 0)
    {
        if ((m_state & MaxReaders) == 0)
        {
            m_readers_left.NotifyOne();
        }
    }
    else if (previous == MaxReaders)
    {
        m_gate.NotifyOne();
    }
}

} // namespace __detail

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
