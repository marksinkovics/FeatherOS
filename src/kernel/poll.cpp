#include "poll.h"

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Poll
{
namespace
{

constinit Sync::SpinLock poll_lock;
constinit Scheduler::WaitQueue poll_waiters;
uint64_t generation = 0;
uint32_t waiting = 0;

} // namespace

void Notify()
{
    Sync::SpinLockGuard guard(poll_lock);
    ++generation;
    if (waiting != 0)
    {
        Scheduler::WakeAll(poll_waiters);
    }
}

uint64_t Generation()
{
    Sync::SpinLockGuard guard(poll_lock);
    return generation;
}

void WaitForChange(uint64_t seen, uint64_t deadline_tick)
{
    Sync::SpinLockGuard guard(poll_lock);
    if (generation != seen)
    {
        return;
    }
    ++waiting;
    Scheduler::WaitOn(poll_waiters, poll_lock, deadline_tick);
    --waiting;
}

} // namespace FeatherOS::Poll
