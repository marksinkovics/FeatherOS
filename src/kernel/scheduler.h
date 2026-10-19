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

// Identified by address only; waiters are tracked in the task table.
struct WaitQueue
{
};

bool Initialize();
[[noreturn]] void RunSecondaryCpu();
bool CreateTask(TaskEntry entry, void* context, TaskId* task_id);
bool Yield();
bool Wait(TaskId task_id, uint64_t* exit_status);
bool ReapTask(TaskId task_id);
void EnablePreemption();
void DisablePreemption();
void OnTimerTick(uint64_t tick);
TaskState GetState(TaskId task_id);

void WaitOn(WaitQueue& queue, Sync::SpinLock& lock, uint64_t deadline_tick = 0);
void WakeAll(WaitQueue& queue);
bool SleepUntil(uint64_t tick);

// Reactivates the running task's user address space, or the kernel's outside user tasks.
void ActivateCurrentAddressSpace();
void* CurrentUserContext();
void* SetCurrentUserContext(void* context);
uint64_t CurrentKernelStackTop();

} // namespace FeatherOS::Scheduler

#endif
