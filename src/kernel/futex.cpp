#include "futex.h"

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "user/syscall_abi.h"

namespace FeatherOS::Futex
{
namespace
{

// Lives on the waiting thread's kernel stack while it waits.
struct Waiter
{
    Waiter* next;
    const void* owner;
    uint64_t address;
    Scheduler::TaskId task;
    bool woken;
};

constinit Sync::SpinLock futex_lock;
// Every waiter blocks on this queue, but wakes are aimed at single tasks.
constinit Scheduler::WaitQueue futex_queue;
Waiter* first_waiter = nullptr;
Waiter* last_waiter = nullptr;

void Append(Waiter* waiter)
{
    waiter->next = nullptr;
    if (last_waiter == nullptr)
    {
        first_waiter = waiter;
    }
    else
    {
        last_waiter->next = waiter;
    }
    last_waiter = waiter;
}

void Remove(Waiter* waiter, Waiter* previous)
{
    (previous == nullptr ? first_waiter : previous->next) = waiter->next;
    if (last_waiter == waiter)
    {
        last_waiter = previous;
    }
}

void RemoveWaiter(Waiter* waiter)
{
    Waiter* previous = nullptr;
    for (Waiter* candidate = first_waiter; candidate != nullptr; candidate = candidate->next)
    {
        if (candidate == waiter)
        {
            Remove(waiter, previous);
            return;
        }
        previous = candidate;
    }
}

} // namespace

uint64_t Wait(const void* owner, uint64_t address, uint32_t expected, uint64_t deadline_tick,
              ReadWord read, Interrupted interrupted)
{
    const uint64_t flags = futex_lock.LockIrqSave();
    uint32_t value = 0;
    if (!read(owner, address, &value))
    {
        futex_lock.UnlockIrqRestore(flags);
        return User::SyscallErrorBadAddress;
    }
    if (value != expected)
    {
        futex_lock.UnlockIrqRestore(flags);
        return User::SyscallErrorWouldBlock;
    }
    Waiter self = {nullptr, owner, address, Scheduler::CurrentTaskId(), false};
    Append(&self);
    uint64_t result = 0;
    // Other wakeups are spurious: only Wake sets woken.
    while (!self.woken)
    {
        if (interrupted != nullptr && interrupted())
        {
            result = User::SyscallErrorInterrupted;
            break;
        }
        if (deadline_tick != 0 && Time::TickCount() >= deadline_tick)
        {
            result = User::SyscallErrorTimedOut;
            break;
        }
        Scheduler::WaitOn(futex_queue, futex_lock, deadline_tick);
    }
    if (!self.woken)
    {
        RemoveWaiter(&self);
    }
    else
    {
        result = 0;
    }
    futex_lock.UnlockIrqRestore(flags);
    return result;
}

uint64_t Wake(const void* owner, uint64_t address, uint64_t count)
{
    Sync::SpinLockGuard guard(futex_lock);
    uint64_t woken = 0;
    Waiter* previous = nullptr;
    for (Waiter* waiter = first_waiter; waiter != nullptr && woken < count;)
    {
        Waiter* next = waiter->next;
        if (waiter->owner == owner && waiter->address == address)
        {
            Remove(waiter, previous);
            waiter->woken = true;
            Scheduler::WakeTask(waiter->task);
            ++woken;
        }
        else
        {
            previous = waiter;
        }
        waiter = next;
    }
    return woken;
}

} // namespace FeatherOS::Futex
