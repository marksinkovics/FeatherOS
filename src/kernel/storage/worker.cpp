#include "worker.h"

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "user/thread_safety.h"

#include <stdint.h>

namespace FeatherOS::Storage::Worker
{
namespace
{

constexpr uint32_t QueueCapacity = 8;

struct Entry
{
    Job job;
    void* context;
};

constinit Sync::SpinLock queue_lock;
Scheduler::WaitQueue queue_wait;
Entry queue[QueueCapacity] GUARDED_BY(queue_lock) = {};
uint32_t head GUARDED_BY(queue_lock) = 0;
uint32_t count GUARDED_BY(queue_lock) = 0;
bool started GUARDED_BY(queue_lock) = false;

uint64_t WorkerTask(void*)
{
    for (;;)
    {
        {
            Sync::SpinLockGuard guard(queue_lock);
            while (count == 0)
            {
                Scheduler::WaitOn(queue_wait, queue_lock);
            }
        }
        RunNext();
    }
}

} // namespace

bool Start()
{
    {
        Sync::SpinLockGuard guard(queue_lock);
        if (started)
        {
            return true;
        }
        started = true;
    }
    Scheduler::TaskId task;
    const bool created = Scheduler::CreateTask(WorkerTask, nullptr, &task) &&
                         Scheduler::SetPriority(task, Scheduler::TaskPriority::Normal);
    if (!created)
    {
        Sync::SpinLockGuard guard(queue_lock);
        started = false;
    }
    return created;
}

bool Post(Job job, void* context)
{
    Sync::SpinLockGuard guard(queue_lock);
    if (count == QueueCapacity)
    {
        return false;
    }
    queue[(head + count) % QueueCapacity] = {job, context};
    ++count;
    Scheduler::WakeAll(queue_wait);
    return true;
}

bool RunNext()
{
    Entry entry;
    {
        Sync::SpinLockGuard guard(queue_lock);
        if (count == 0)
        {
            return false;
        }
        entry = queue[head];
        head = (head + 1) % QueueCapacity;
        --count;
    }
    entry.job(entry.context);
    return true;
}

} // namespace FeatherOS::Storage::Worker
