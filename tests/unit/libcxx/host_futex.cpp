#include "libc/lock.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

// atomic_wait.cpp sleeps through the C library's futex calls, which the host has no system call
// for. One mutex and condition variable stand in: the word is checked under the mutex that every
// wake takes, so a wake after the word changes is not missed, and the extra wakeups are allowed.
namespace
{

std::mutex futex_mutex;
std::condition_variable futex_condition;

} // namespace

namespace FeatherOS::LibC
{

uint64_t FutexWait(int* word, int expected, uint64_t milliseconds)
{
    std::unique_lock lock(futex_mutex);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected)
    {
        return 1;
    }
    if (milliseconds == 0)
    {
        futex_condition.wait(lock);
    }
    else
    {
        futex_condition.wait_for(lock, std::chrono::milliseconds(milliseconds));
    }
    return 0;
}

void FutexWake(int*, uint64_t)
{
    const std::lock_guard lock(futex_mutex);
    futex_condition.notify_all();
}

} // namespace FeatherOS::LibC
