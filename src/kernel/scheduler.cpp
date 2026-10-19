#include "scheduler.h"

#include "kernel/arch/interrupts.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/panic.h"
#include "kernel/time/timer.h"
#include "kernel/user_context.h"

#include <stddef.h>

namespace FeatherOS::Scheduler
{
namespace
{

constexpr size_t MaximumTasks = 4;
constexpr size_t TaskStackSize = 16384;

struct Task
{
    uint64_t stack_pointer;
    TaskEntry entry;
    void* context;
    TaskId id;
    TaskState state;
    uint64_t exit_status;
    // 0 = no deadline.
    uint64_t wake_tick;
    const WaitQueue* wait_queue;
    void* user_context;
    // Null for the boot task, which runs on the bootloader's stack.
    uint8_t* stack;
    WaitQueue exit_queue;
};

// Lock order: subsystem locks before the scheduler lock. It is held across context switches and
// released by the task that switches in.
Sync::SpinLock scheduler_lock;
Task tasks[MaximumTasks];
alignas(16) uint8_t task_stacks[MaximumTasks][TaskStackSize];
Task boot_task;
Task idle_task;
alignas(16) uint8_t idle_stack[TaskStackSize];
Task* current_task = nullptr;
TaskId next_task_id = 1;
bool initialized = false;
volatile bool preemption_enabled = false;

extern "C" void FeatherOS_Arch_SwitchContext(uint64_t* old_stack_pointer,
                                             uint64_t new_stack_pointer);

[[noreturn]] void TaskReturnFailure()
{
    FeatherOS::Panic("Cooperative task returned through an invalid stack frame");
}

void ResetTask(Task& task)
{
    task.stack_pointer = 0;
    task.entry = nullptr;
    task.context = nullptr;
    task.id = 0;
    task.state = TaskState::Empty;
    task.exit_status = 0;
    task.wake_tick = 0;
    task.wait_queue = nullptr;
    task.user_context = nullptr;
}

constexpr size_t RunOrderLength = MaximumTasks + 1;

Task* RunOrderTask(size_t position)
{
    return position < MaximumTasks ? &tasks[position] : &boot_task;
}

size_t RunOrderPosition(const Task* task)
{
    for (size_t position = 0; position < RunOrderLength; ++position)
    {
        if (RunOrderTask(position) == task)
        {
            return position;
        }
    }
    return RunOrderLength - 1;
}

Task* FindTask(TaskId task_id)
{
    if (task_id == 0)
    {
        return nullptr;
    }
    for (Task& task : tasks)
    {
        if (task.id == task_id && task.state != TaskState::Empty)
        {
            return &task;
        }
    }
    return nullptr;
}

Task* FindReadyTask(const Task* after)
{
    const size_t start = (RunOrderPosition(after) + 1) % RunOrderLength;
    for (size_t offset = 0; offset < RunOrderLength; ++offset)
    {
        Task* candidate = RunOrderTask((start + offset) % RunOrderLength);
        if (candidate != after && candidate->state == TaskState::Ready)
        {
            return candidate;
        }
    }
    return nullptr;
}

void MakeReady(Task& task)
{
    task.state = TaskState::Ready;
    task.wake_tick = 0;
    task.wait_queue = nullptr;
}

void WakeAllLocked(const WaitQueue& queue)
{
    for (size_t position = 0; position < RunOrderLength; ++position)
    {
        Task* task = RunOrderTask(position);
        if (task->state == TaskState::Blocked && task->wait_queue == &queue)
        {
            MakeReady(*task);
        }
    }
}

void ActivateTaskAddressSpace(const Task* task)
{
    auto* user_context =
        task == nullptr ? nullptr : static_cast<UserExecutionContext*>(task->user_context);
    if (user_context != nullptr && user_context->active)
    {
        const Memory::AddressSpace address_space = {
            user_context->address_space_root_physical_address,
        };
        if (!Memory::ActivateAddressSpace(address_space))
        {
            FeatherOS::Panic("Could not restore task user address space");
        }
    }
    else
    {
        Memory::ActivateKernelAddressSpace();
    }
}

bool Schedule()
{
    Task* previous = current_task;
    Task* next = FindReadyTask(previous);
    if (next == nullptr)
    {
        if (previous->state == TaskState::Ready)
        {
            previous->state = TaskState::Running;
            return false;
        }
        if (previous == &idle_task)
        {
            return false;
        }
        next = &idle_task;
    }
    else
    {
        next->state = TaskState::Running;
    }
    current_task = next;
    ActivateTaskAddressSpace(next);
    FeatherOS_Arch_SwitchContext(&previous->stack_pointer, next->stack_pointer);
    return true;
}

// Entered from a context switch, so the scheduler lock is still held.
void TaskBootstrap()
{
    scheduler_lock.Unlock();
    Arch::Interrupts::Enable();

    Task* task = current_task;
    if (task == nullptr || task->entry == nullptr)
    {
        FeatherOS::Panic("Task bootstrap has no current task");
    }
    const uint64_t exit_status = task->entry(task->context);

    Arch::Interrupts::SaveAndDisable();
    scheduler_lock.Lock();
    task->exit_status = exit_status;
    task->state = TaskState::Complete;
    WakeAllLocked(task->exit_queue);
    Schedule();
    TaskReturnFailure();
}

void PrepareTaskStack(Task& task, uint8_t* stack)
{
    task.stack = stack;
    uintptr_t stack_top = reinterpret_cast<uintptr_t>(stack + TaskStackSize);
    stack_top &= ~static_cast<uintptr_t>(0xF);

#if defined(ARCH_x86_64)
    auto* frame = reinterpret_cast<uint64_t*>(stack_top) - 8;
    for (size_t index = 0; index < 6; ++index)
    {
        frame[index] = 0;
    }
    frame[6] = reinterpret_cast<uint64_t>(TaskBootstrap);
    frame[7] = reinterpret_cast<uint64_t>(TaskReturnFailure);
    task.stack_pointer = reinterpret_cast<uint64_t>(frame);
#elif defined(ARCH_aarch64)
    // Saved SP_EL0 and padding, x19-x28, x29, then x30 as the bootstrap return address.
    auto* frame = reinterpret_cast<uint64_t*>(stack_top) - 14;
    for (size_t index = 0; index < 12; ++index)
    {
        frame[index] = 0;
    }
    frame[12] = 0;
    frame[13] = reinterpret_cast<uint64_t>(TaskBootstrap);
    task.stack_pointer = reinterpret_cast<uint64_t>(frame);
#else
#error "Unsupported scheduler architecture"
#endif
}

uint64_t IdleTaskMain(void*)
{
    for (;;)
    {
        Arch::Interrupts::SaveAndDisable();
        scheduler_lock.Lock();
        const bool work_ready = FindReadyTask(&idle_task) != nullptr;
        scheduler_lock.Unlock();
        if (work_ready)
        {
            Arch::Interrupts::Enable();
            Yield();
        }
        else
        {
            Arch::Interrupts::WaitForInterrupt();
        }
    }
}

} // namespace

bool Initialize()
{
    const uint64_t flags = scheduler_lock.LockIrqSave();
    for (Task& task : tasks)
    {
        ResetTask(task);
    }
    ResetTask(boot_task);
    boot_task.state = TaskState::Running;
    boot_task.stack = nullptr;
    ResetTask(idle_task);
    idle_task.entry = IdleTaskMain;
    idle_task.state = TaskState::Running;
    PrepareTaskStack(idle_task, idle_stack);
    current_task = &boot_task;
    next_task_id = 1;
    preemption_enabled = false;
    initialized = true;
    scheduler_lock.UnlockIrqRestore(flags);
    return true;
}

bool CreateTask(TaskEntry entry, void* context, TaskId* task_id)
{
    if (!initialized || entry == nullptr || task_id == nullptr)
    {
        return false;
    }

    Sync::SpinLockGuard guard(scheduler_lock);
    if (next_task_id == 0)
    {
        return false;
    }
    size_t task_index = 0;
    while (task_index < MaximumTasks && tasks[task_index].state != TaskState::Empty)
    {
        ++task_index;
    }
    if (task_index == MaximumTasks)
    {
        return false;
    }

    Task& task = tasks[task_index];
    ResetTask(task);
    task.entry = entry;
    task.context = context;
    task.id = next_task_id++;
    PrepareTaskStack(task, task_stacks[task_index]);
    task.state = TaskState::Ready;
    *task_id = task.id;
    return true;
}

bool Yield()
{
    if (!initialized)
    {
        return false;
    }

    const uint64_t flags = scheduler_lock.LockIrqSave();
    if (current_task != &idle_task && current_task->state == TaskState::Running)
    {
        current_task->state = TaskState::Ready;
    }
    const bool switched = Schedule();
    scheduler_lock.UnlockIrqRestore(flags);
    return switched;
}

void EnablePreemption()
{
    preemption_enabled = true;
}

void DisablePreemption()
{
    preemption_enabled = false;
}

void OnTimerTick(uint64_t tick)
{
    if (!initialized)
    {
        return;
    }

    // Interrupt handlers run with interrupts masked.
    scheduler_lock.Lock();
    for (size_t position = 0; position < RunOrderLength; ++position)
    {
        Task* task = RunOrderTask(position);
        if ((task->state == TaskState::Sleeping || task->state == TaskState::Blocked) &&
            task->wake_tick != 0 && tick >= task->wake_tick)
        {
            MakeReady(*task);
        }
    }
    if (current_task == &idle_task || preemption_enabled)
    {
        if (current_task != &idle_task && current_task->state == TaskState::Running)
        {
            current_task->state = TaskState::Ready;
        }
        Schedule();
    }
    scheduler_lock.Unlock();
}

bool Wait(TaskId task_id, uint64_t* exit_status)
{
    if (!initialized)
    {
        return false;
    }

    const uint64_t flags = scheduler_lock.LockIrqSave();
    Task* task = FindTask(task_id);
    while (task != nullptr && task != current_task && task->state != TaskState::Complete)
    {
        current_task->state = TaskState::Blocked;
        current_task->wait_queue = &task->exit_queue;
        current_task->wake_tick = 0;
        Schedule();
        // Another waiter may have reaped the task while this one was blocked.
        task = FindTask(task_id);
    }
    const bool completed = task != nullptr && task != current_task;
    if (completed && exit_status != nullptr)
    {
        *exit_status = task->exit_status;
    }
    scheduler_lock.UnlockIrqRestore(flags);
    return completed;
}

bool ReapTask(TaskId task_id)
{
    if (!initialized)
    {
        return false;
    }

    Sync::SpinLockGuard guard(scheduler_lock);
    Task* task = FindTask(task_id);
    if (task == nullptr || task->state != TaskState::Complete || task == current_task)
    {
        return false;
    }
    ResetTask(*task);
    return true;
}

TaskState GetState(TaskId task_id)
{
    if (!initialized)
    {
        return TaskState::Empty;
    }

    Sync::SpinLockGuard guard(scheduler_lock);
    const Task* task = FindTask(task_id);
    return task == nullptr ? TaskState::Empty : task->state;
}

void WaitOn(WaitQueue& queue, Sync::SpinLock& lock, uint64_t deadline_tick)
{
    if (!initialized || current_task == &idle_task)
    {
        // Without a scheduler, any interrupt counts as a spurious wakeup.
        lock.Unlock();
        Arch::Interrupts::WaitForInterrupt();
        Arch::Interrupts::SaveAndDisable();
        lock.Lock();
        return;
    }

    scheduler_lock.Lock();
    if (deadline_tick != 0 && Time::TickCount() >= deadline_tick)
    {
        scheduler_lock.Unlock();
        return;
    }
    current_task->state = TaskState::Blocked;
    current_task->wait_queue = &queue;
    current_task->wake_tick = deadline_tick;
    lock.Unlock();
    Schedule();
    scheduler_lock.Unlock();
    lock.Lock();
}

void WakeAll(WaitQueue& queue)
{
    if (!initialized)
    {
        return;
    }

    Sync::SpinLockGuard guard(scheduler_lock);
    WakeAllLocked(queue);
}

bool SleepUntil(uint64_t tick)
{
    if (!initialized || current_task == &idle_task)
    {
        return false;
    }

    const uint64_t flags = scheduler_lock.LockIrqSave();
    if (Time::TickCount() < tick)
    {
        current_task->state = TaskState::Sleeping;
        current_task->wake_tick = tick;
        Schedule();
    }
    scheduler_lock.UnlockIrqRestore(flags);
    return true;
}

void ActivateCurrentAddressSpace()
{
    ActivateTaskAddressSpace(current_task);
}

void* CurrentUserContext()
{
    return current_task == nullptr ? nullptr : current_task->user_context;
}

void* SetCurrentUserContext(void* context)
{
    if (current_task == nullptr)
    {
        return nullptr;
    }
    const uint64_t flags = Arch::Interrupts::SaveAndDisable();
    void* previous_context = current_task->user_context;
    current_task->user_context = context;
    Arch::Interrupts::Restore(flags);
    return previous_context;
}

uint64_t CurrentKernelStackTop()
{
    if (current_task == nullptr || current_task->stack == nullptr)
    {
        return 0;
    }
    uintptr_t stack_top = reinterpret_cast<uintptr_t>(current_task->stack + TaskStackSize);
    stack_top &= ~static_cast<uintptr_t>(0xF);
    return stack_top;
}

} // namespace FeatherOS::Scheduler
