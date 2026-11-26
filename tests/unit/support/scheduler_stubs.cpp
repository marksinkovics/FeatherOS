#include "support/scheduler_stubs.h"

#include "kernel/arch/interrupts.h"
#include "kernel/scheduler.h"
#include "kernel/sync/mutex.h"
#include "kernel/time/timer.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

namespace TestScheduler
{

std::atomic<int> wait_count = 0;
std::atomic<int> wake_count = 0;
std::atomic<int> early_wakes = 0;
std::atomic<uint64_t> last_wait_deadline = 0;
std::atomic<bool> lock_held_during_wait = false;
std::atomic<int> volume_violations = 0;
std::function<void()> on_wait;
std::vector<uint32_t> woken_tasks;
thread_local uint32_t current_task = 1;
std::atomic<uint64_t> ticks = 0;

namespace
{
std::mutex woken_mutex;
std::mutex wake_mutex;
std::condition_variable wake_signal;
uint64_t wake_generation = 0;
// Both under wake_mutex: tasks inside WaitOn (by count) and tasks holding a wake-up.
std::map<uint32_t, int> sleeping;
std::set<uint32_t> tokens;
thread_local FeatherOS::Scheduler::TaskLocal task_local{};
} // namespace

void Reset()
{
    wait_count = 0;
    wake_count = 0;
    early_wakes = 0;
    {
        const std::lock_guard guard(wake_mutex);
        sleeping.clear();
        tokens.clear();
    }
    volume_violations = 0;
    task_local = {};
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
    std::unique_lock<std::mutex> wake_guard(TestScheduler::wake_mutex);
    if (TestScheduler::tokens.erase(TestScheduler::current_task) != 0)
    {
        ++TestScheduler::early_wakes;
        return;
    }
    const uint64_t generation = TestScheduler::wake_generation;
    ++TestScheduler::sleeping[TestScheduler::current_task];
    lock.Unlock();
    if (TestScheduler::on_wait)
    {
        wake_guard.unlock();
        TestScheduler::on_wait();
    }
    else
    {
        TestScheduler::wake_signal.wait_for(
            wake_guard, std::chrono::milliseconds(1),
            [generation] { return TestScheduler::wake_generation != generation; });
        wake_guard.unlock();
    }
    {
        const std::lock_guard guard(TestScheduler::wake_mutex);
        if (--TestScheduler::sleeping[TestScheduler::current_task] == 0)
        {
            TestScheduler::sleeping.erase(TestScheduler::current_task);
        }
    }
    lock.Lock();
}

void WakeAll(WaitQueue&)
{
    ++TestScheduler::wake_count;
    {
        const std::lock_guard guard(TestScheduler::wake_mutex);
        ++TestScheduler::wake_generation;
    }
    TestScheduler::wake_signal.notify_all();
}

void WakeTask(TaskId task_id)
{
    {
        const std::lock_guard guard(TestScheduler::woken_mutex);
        TestScheduler::woken_tasks.push_back(task_id);
    }
    bool wake_sleeper = false;
    {
        const std::lock_guard guard(TestScheduler::wake_mutex);
        if (TestScheduler::sleeping.contains(task_id))
        {
            ++TestScheduler::wake_generation;
            wake_sleeper = true;
        }
        else
        {
            TestScheduler::tokens.insert(task_id);
        }
    }
    if (wake_sleeper)
    {
        TestScheduler::wake_signal.notify_all();
    }
}

TaskId CurrentTaskId()
{
    return TestScheduler::current_task;
}

Scheduler::TaskLocal& CurrentLocal()
{
    return TestScheduler::task_local;
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

namespace FeatherOS::Sync
{

void VolumeLockViolation()
{
    ++TestScheduler::volume_violations;
}

} // namespace FeatherOS::Sync

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

uint64_t UptimeMilliseconds()
{
    return TestScheduler::ticks * 10;
}

} // namespace FeatherOS::Time

// Tests run on one thread with nothing to mask.
namespace FeatherOS::Arch::Interrupts
{

uint64_t SaveAndDisable()
{
    return 0;
}

void Restore(uint64_t)
{
}

void Enable()
{
}

} // namespace FeatherOS::Arch::Interrupts
