#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/thread"
#else
#include <thread>
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace __detail
{

Condition::Condition() noexcept
{
#ifdef __APPLE__
    // macOS, where only the host tests run, has no pthread_condattr_setclock; WaitUntil waits
    // for a relative time there instead.
    pthread_cond_init(&m_handle, nullptr);
#else
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    pthread_cond_init(&m_handle, &attributes);
    pthread_condattr_destroy(&attributes);
#endif
}

Condition::~Condition()
{
    pthread_cond_destroy(&m_handle);
}

void Condition::Wait(pthread_mutex_t* mutex) noexcept
{
    pthread_cond_wait(&m_handle, mutex);
}

bool Condition::WaitUntil(pthread_mutex_t* mutex,
                          chrono::steady_clock::time_point deadline) noexcept
{
    // The C library's millisecond rounding would overflow this close to the end of time.
    if (deadline >= chrono::steady_clock::time_point::max() - chrono::hours(1))
    {
        Wait(mutex);
        return true;
    }
#ifdef __APPLE__
    const chrono::nanoseconds remaining = deadline - chrono::steady_clock::now();
    if (remaining <= chrono::nanoseconds::zero())
    {
        return false;
    }
    const timespec relative{static_cast<time_t>(remaining.count() / 1'000'000'000),
                            static_cast<long>(remaining.count() % 1'000'000'000)};
    return pthread_cond_timedwait_relative_np(&m_handle, mutex, &relative) != ETIMEDOUT;
#else
    const long long nanoseconds = deadline.time_since_epoch().count();
    const timespec absolute{static_cast<time_t>(nanoseconds / 1'000'000'000),
                            static_cast<long>(nanoseconds % 1'000'000'000)};
    if (absolute.tv_nsec < 0)
    {
        return false;
    }
    return pthread_cond_timedwait(&m_handle, mutex, &absolute) != ETIMEDOUT;
#endif
}

void Condition::NotifyOne() noexcept
{
    pthread_cond_signal(&m_handle);
}

void Condition::NotifyAll() noexcept
{
    pthread_cond_broadcast(&m_handle);
}

void Sleep(chrono::nanoseconds duration) noexcept
{
    timespec remaining{static_cast<time_t>(duration.count() / 1'000'000'000),
                       static_cast<long>(duration.count() % 1'000'000'000)};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
    {
    }
}

} // namespace __detail

thread::~thread()
{
    if (joinable())
    {
        __detail::LibraryAbort("thread: destroyed while joinable");
    }
}

thread& thread::operator=(thread&& other) noexcept
{
    if (joinable())
    {
        __detail::LibraryAbort("thread: assigned to while joinable");
    }
    m_id = _FSTD::exchange(other.m_id, id());
    return *this;
}

void thread::join()
{
    if (!joinable())
    {
        __detail::LibraryAbort("thread::join: not joinable");
    }
    if (m_id == this_thread::get_id())
    {
        __detail::LibraryAbort("thread::join: a thread cannot join itself");
    }
    if (pthread_join(m_id.m_handle, nullptr) != 0)
    {
        __detail::LibraryAbort("thread::join: pthread_join failed");
    }
    m_id = id();
}

void thread::detach()
{
    if (!joinable())
    {
        __detail::LibraryAbort("thread::detach: not joinable");
    }
    if (pthread_detach(m_id.m_handle) != 0)
    {
        __detail::LibraryAbort("thread::detach: pthread_detach failed");
    }
    m_id = id();
}

thread::id thread::Start(void* (*run)(void*), void* state)
{
    pthread_t handle{};
    if (pthread_create(&handle, nullptr, run, state) != 0)
    {
        __detail::LibraryAbort("thread: cannot create a thread");
    }
    return id(handle);
}

unsigned int thread::hardware_concurrency() noexcept
{
    const long processors = sysconf(_SC_NPROCESSORS_ONLN);
    return processors > 0 ? static_cast<unsigned int>(processors) : 0;
}

void this_thread::yield() noexcept
{
    sched_yield();
}

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
