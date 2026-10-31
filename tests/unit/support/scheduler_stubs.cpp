#include "support/scheduler_stubs.h"

#include "kernel/scheduler.h"
#include "kernel/time/timer.h"

namespace TestScheduler
{

int wait_count = 0;
int wake_count = 0;
uint64_t last_wait_deadline = 0;
bool lock_held_during_wait = false;
std::function<void()> on_wait;
std::vector<uint32_t> woken_tasks;
uint32_t current_task = 1;
uint64_t ticks = 0;

void Reset()
{
    wait_count = 0;
    wake_count = 0;
    last_wait_deadline = 0;
    lock_held_during_wait = false;
    on_wait = nullptr;
    woken_tasks.clear();
    current_task = 1;
    ticks = 0;
}

} // namespace TestScheduler

namespace FeatherOS::Scheduler
{

void WaitOn(WaitQueue&, Sync::SpinLock& lock, uint64_t deadline_tick)
{
    ++TestScheduler::wait_count;
    TestScheduler::last_wait_deadline = deadline_tick;
    TestScheduler::lock_held_during_wait = lock.IsLocked();
    if (TestScheduler::on_wait)
    {
        lock.Unlock();
        TestScheduler::on_wait();
        lock.Lock();
    }
}

void WakeAll(WaitQueue&)
{
    ++TestScheduler::wake_count;
}

void WakeTask(TaskId task_id)
{
    TestScheduler::woken_tasks.push_back(task_id);
}

TaskId CurrentTaskId()
{
    return TestScheduler::current_task;
}

// Tests run the work of kernel tasks by hand, so a task is never started.
bool CreateTask(TaskEntry, void*, TaskId* task_id)
{
    *task_id = 0;
    return true;
}

bool SetPriority(TaskId, TaskPriority)
{
    return true;
}

} // namespace FeatherOS::Scheduler

namespace FeatherOS::Time
{

uint64_t TickCount()
{
    return TestScheduler::ticks;
}

uint32_t TicksPerSecond()
{
    return 100;
}

} // namespace FeatherOS::Time
