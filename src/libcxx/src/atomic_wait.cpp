#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/atomic"
#include "libcxx/include/chrono"
#else
#include <atomic>
#include <chrono>
#endif

#include "libc/lock.h"

#include <stdint.h>

// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE::__detail
{

namespace
{

// notifications counts the notifications for the slot's addresses and is the futex word: a
// waiter reads it before polling and sleeps only while it is unchanged, so a notification
// between the poll and the sleep is not lost. waiters spares a notification with no one waiting
// the system call; both are sequentially consistent, so either the notifier sees a waiter, or
// the waiter reads the new count and with it the value the notifier stored.
struct alignas(64) WaitSlot
{
    int notifications;
    int waiters;
};

constexpr size_t SlotCount = 64;
constinit WaitSlot slots[SlotCount] = {};

WaitSlot& SlotFor(const void* address) noexcept
{
    const uintptr_t bits = reinterpret_cast<uintptr_t>(address);
    return slots[((bits >> 2) ^ (bits >> 8)) % SlotCount];
}

// 0 would wait forever, so a wait always asks for at least a millisecond.
uint64_t MillisecondsUntil(int64_t deadline, int64_t now) noexcept
{
    constexpr int64_t NanosecondsPerMillisecond = 1000000;
    const int64_t left = deadline - now;
    return static_cast<uint64_t>(left / NanosecondsPerMillisecond +
                                 (left % NanosecondsPerMillisecond != 0 ? 1 : 0));
}

} // namespace

bool AtomicWaitSlow(const void* address, AtomicPoll poll, void* context, int64_t deadline) noexcept
{
    WaitSlot& slot = SlotFor(address);
    __atomic_fetch_add(&slot.waiters, 1, __ATOMIC_SEQ_CST);
    bool changed = false;
    while (true)
    {
        const int seen = __atomic_load_n(&slot.notifications, __ATOMIC_SEQ_CST);
        if (poll(context))
        {
            changed = true;
            break;
        }
        uint64_t milliseconds = 0;
        if (deadline != AtomicWaitForever)
        {
            const int64_t now = chrono::steady_clock::now().time_since_epoch().count();
            if (now >= deadline)
            {
                break;
            }
            milliseconds = MillisecondsUntil(deadline, now);
        }
        FeatherOS::LibC::FutexWait(&slot.notifications, seen, milliseconds);
    }
    __atomic_fetch_sub(&slot.waiters, 1, __ATOMIC_RELAXED);
    return changed;
}

void AtomicNotify(const void* address) noexcept
{
    WaitSlot& slot = SlotFor(address);
    __atomic_fetch_add(&slot.notifications, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&slot.waiters, __ATOMIC_SEQ_CST) != 0)
    {
        FeatherOS::LibC::FutexWake(&slot.notifications, UINT64_MAX);
    }
}

} // namespace FEATHEROS_STD_NAMESPACE::__detail
// NOLINTEND(bugprone-std-namespace-modification)
