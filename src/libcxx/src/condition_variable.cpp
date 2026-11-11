#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/condition_variable"
#else
#include <condition_variable>
#endif

// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace
{

struct ExitNotification
{
    condition_variable* condition;
    mutex* lock;
    ExitNotification* next;
};

class ExitNotifications
{
public:
    ExitNotifications() = default;
    ExitNotifications(const ExitNotifications&) = delete;
    ExitNotifications& operator=(const ExitNotifications&) = delete;

    // The mutexes were locked by this thread in notify_all_at_thread_exit's callers.
    ~ExitNotifications() FEATHEROS_NO_THREAD_SAFETY_ANALYSIS
    {
        while (m_head != nullptr)
        {
            ExitNotification* notification = m_head;
            m_head = notification->next;
            notification->lock->unlock();
            notification->condition->notify_all();
            delete notification;
        }
    }

    void Add(condition_variable* condition, mutex* lock)
    {
        m_head = new ExitNotification{condition, lock, m_head};
    }

private:
    ExitNotification* m_head = nullptr;
};

thread_local ExitNotifications exit_notifications;

} // namespace

void condition_variable::wait(unique_lock<mutex>& lock)
{
    m_condition.Wait(lock.mutex()->native_handle());
}

void condition_variable::WaitUntil(unique_lock<mutex>& lock,
                                   chrono::steady_clock::time_point deadline)
{
    m_condition.WaitUntil(lock.mutex()->native_handle(), deadline);
}

void notify_all_at_thread_exit(condition_variable& condition, unique_lock<mutex> lock)
{
    exit_notifications.Add(&condition, lock.release());
}

condition_variable_any::condition_variable_any() : m_mutex(make_shared<mutex>()) {}

// Taking the internal mutex waits out any waiter between unlocking its own lock and waiting.
void condition_variable_any::notify_one() noexcept
{
    {
        const lock_guard<mutex> guard(*m_mutex);
    }
    m_condition.notify_one();
}

void condition_variable_any::notify_all() noexcept
{
    {
        const lock_guard<mutex> guard(*m_mutex);
    }
    m_condition.notify_all();
}

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
