#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/stop_token"
#else
#include <stop_token>
#endif

#include <pthread.h>
#include <sched.h>

// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE::__detail
{

void StopState::Release() noexcept
{
    if (m_references.fetch_sub(1, memory_order_acq_rel) == 1)
    {
        delete this;
    }
}

void StopState::Lock() noexcept
{
    while (m_lock.test_and_set(memory_order_acquire))
    {
        sched_yield();
    }
}

void StopState::Unlock() noexcept
{
    m_lock.clear(memory_order_release);
}

bool StopState::RequestStop() noexcept
{
    Lock();
    if (m_requested.load(memory_order_relaxed))
    {
        Unlock();
        return false;
    }
    m_requested.store(true, memory_order_release);
    m_requester = pthread_self();
    while (m_callbacks != nullptr)
    {
        StopCallbackBase* callback = m_callbacks;
        m_callbacks = callback->m_next;
        if (m_callbacks != nullptr)
        {
            m_callbacks->m_link = &m_callbacks;
        }
        callback->m_link = nullptr;
        m_running = callback;
        bool destroyed = false;
        callback->m_destroyed = &destroyed;
        Unlock();
        callback->m_run(callback);
        // Once m_finished is set, the callback's owner may free it at once.
        if (!destroyed)
        {
            callback->m_destroyed = nullptr;
            callback->m_finished.store(true, memory_order_release);
        }
        Lock();
        m_running = nullptr;
    }
    Unlock();
    return true;
}

bool StopState::AddCallback(StopCallbackBase* callback) noexcept
{
    Lock();
    if (m_requested.load(memory_order_relaxed))
    {
        Unlock();
        return false;
    }
    callback->m_next = m_callbacks;
    if (m_callbacks != nullptr)
    {
        m_callbacks->m_link = &callback->m_next;
    }
    callback->m_link = &m_callbacks;
    m_callbacks = callback;
    Unlock();
    return true;
}

void StopState::RemoveCallback(StopCallbackBase* callback) noexcept
{
    Lock();
    if (callback->m_link != nullptr)
    {
        *callback->m_link = callback->m_next;
        if (callback->m_next != nullptr)
        {
            callback->m_next->m_link = callback->m_link;
        }
        Unlock();
        return;
    }
    // A callback that destroys itself while it runs only tells request_stop to leave it alone.
    const bool running_here = m_running == callback && pthread_equal(m_requester, pthread_self());
    if (running_here && callback->m_destroyed != nullptr)
    {
        *callback->m_destroyed = true;
    }
    Unlock();
    if (running_here)
    {
        return;
    }
    while (!callback->m_finished.load(memory_order_acquire))
    {
        sched_yield();
    }
}

} // namespace FEATHEROS_STD_NAMESPACE::__detail
// NOLINTEND(bugprone-std-namespace-modification)
