#ifndef FEATHEROS_SCHEDULER_H
#define FEATHEROS_SCHEDULER_H

#include "kernel/sync/spinlock.h"

#include <stdint.h>

namespace FeatherOS::Scheduler
{

using TaskId = uint32_t;
using TaskEntry = uint64_t (*)(void* context);

enum class TaskState : uint8_t
{
    Empty,
    Ready,
    Running,
    Blocked,
    Sleeping,
    Complete,
};

enum class TaskPriority : uint8_t
{
    Low,
    Normal,
    High,
};

// Identified by address only; waiters are tracked in the task table.
struct WaitQueue
{
};

bool Initialize();
[[noreturn]] void RunSecondaryCpu();
bool CreateTask(TaskEntry entry, void* context, TaskId* task_id);
bool Yield();
// Fails early, without the task having exited, once interrupted returns true; it is checked
// before waiting and after each wakeup, under the scheduler lock, so it must take no locks.
bool Wait(TaskId task_id, uint64_t* exit_status, bool (*interrupted)() = nullptr);
bool ReapTask(TaskId task_id);
bool SetPriority(TaskId task_id, TaskPriority priority);
void EnablePreemption();
void DisablePreemption();
void OnTimerTick(uint64_t tick);
uint64_t BusyTickCount(uint32_t cpu_index);
TaskState GetState(TaskId task_id);
TaskId CurrentTaskId();
// Timer ticks the task has run for, on any CPU, and the CPU it last ran on.
bool GetUsage(TaskId task_id, uint64_t* run_ticks, uint32_t* last_cpu);

void WaitOn(WaitQueue& queue, Sync::SpinLock& lock, uint64_t deadline_tick = 0);
void WakeAll(WaitQueue& queue);
// Ends a sleep or wait early; waiters treat it as a spurious wakeup and recheck their condition.
void WakeTask(TaskId task_id);
bool SleepUntil(uint64_t tick);

// Reactivates the running task's user address space, or the kernel's outside user tasks.
void ActivateCurrentAddressSpace();
void* CurrentUserContext();
void* SetCurrentUserContext(void* context);
uint64_t CurrentKernelStackTop();

} // namespace FeatherOS::Scheduler

#endif
