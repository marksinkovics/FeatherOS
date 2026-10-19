#include "scheduler.h"

#include "kernel/arch/interrupts.h"
#include "kernel/cpu.h"
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
// Secondary CPUs run their idle task on the stack they started on; only the boot CPU needs one.
Task idle_tasks[Cpu::MaximumCpus];
alignas(16) uint8_t boot_idle_stack[TaskStackSize];
Task* current_tasks[Cpu::MaximumCpus];
bool cpu_scheduling[Cpu::MaximumCpus];
TaskId next_task_id = 1;
bool initialized = false;
volatile bool preemption_enabled = false;

extern "C" void FeatherOS_Arch_SwitchContext(uint64_t* old_stack_pointer,
                                             uint64_t new_stack_pointer);

Task*& CurrentTask()
{
    return current_tasks[Cpu::CurrentIndex()];
}

Task& IdleTask()
{
    return idle_tasks[Cpu::CurrentIndex()];
}

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

// An idle CPU halts until an interrupt, so new work would otherwise wait for its next tick.
void WakeIdleCpu()
{
    const uint32_t self = Cpu::CurrentIndex();
    for (uint32_t cpu = 0; cpu < Cpu::MaximumCpus; ++cpu)
    {
        if (cpu != self && cpu_scheduling[cpu] && current_tasks[cpu] == &idle_tasks[cpu])
        {
            Arch::Interrupts::SendIpi(cpu);
            return;
        }
    }
}

void MakeReady(Task& task)
{
    task.state = TaskState::Ready;
    task.wake_tick = 0;
    task.wait_queue = nullptr;
    WakeIdleCpu();
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
    Task* previous = CurrentTask();
    Task* next = FindReadyTask(previous);
    if (next == nullptr)
    {
        if (previous->state == TaskState::Ready)
        {
            previous->state = TaskState::Running;
            return false;
        }
        if (previous == &IdleTask())
        {
            return false;
        }
        next = &IdleTask();
    }
    else
    {
        next->state = TaskState::Running;
    }
    CurrentTask() = next;
    ActivateTaskAddressSpace(next);
    FeatherOS_Arch_SwitchContext(&previous->stack_pointer, next->stack_pointer);
    return true;
}

// Entered from a context switch, so the scheduler lock is still held.
void TaskBootstrap()
{
    scheduler_lock.Unlock();
    Arch::Interrupts::Enable();

    Task* task = CurrentTask();
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
        const bool work_ready = FindReadyTask(&IdleTask()) != nullptr;
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
    Task& idle_task = IdleTask();
    ResetTask(idle_task);
    idle_task.entry = IdleTaskMain;
    idle_task.state = TaskState::Running;
    PrepareTaskStack(idle_task, boot_idle_stack);
    CurrentTask() = &boot_task;
    for (bool& scheduling : cpu_scheduling)
    {
        scheduling = false;
    }
    cpu_scheduling[Cpu::CurrentIndex()] = true;
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
    MakeReady(task);
    *task_id = task.id;
    return true;
}

void RunSecondaryCpu()
{
    if (!initialized)
    {
        Panic("Secondary CPU started before the scheduler");
    }
    Arch::Interrupts::SaveAndDisable();
    scheduler_lock.Lock();
    Task& idle_task = IdleTask();
    ResetTask(idle_task);
    idle_task.entry = IdleTaskMain;
    idle_task.state = TaskState::Running;
    idle_task.stack = nullptr;
    CurrentTask() = &idle_task;
    cpu_scheduling[Cpu::CurrentIndex()] = true;
    scheduler_lock.Unlock();
    IdleTaskMain(nullptr);
    Panic("Secondary CPU idle task returned");
}

bool Yield()
{
    if (!initialized)
    {
        return false;
    }

    const uint64_t flags = scheduler_lock.LockIrqSave();
    if (CurrentTask() != &IdleTask() && CurrentTask()->state == TaskState::Running)
    {
        CurrentTask()->state = TaskState::Ready;
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
    if (!initialized || CurrentTask() == nullptr)
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
    if (CurrentTask() == &IdleTask() || preemption_enabled)
    {
        if (CurrentTask() != &IdleTask() && CurrentTask()->state == TaskState::Running)
        {
            CurrentTask()->state = TaskState::Ready;
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
    while (task != nullptr && task != CurrentTask() && task->state != TaskState::Complete)
    {
        CurrentTask()->state = TaskState::Blocked;
        CurrentTask()->wait_queue = &task->exit_queue;
        CurrentTask()->wake_tick = 0;
        Schedule();
        // Another waiter may have reaped the task while this one was blocked.
        task = FindTask(task_id);
    }
    const bool completed = task != nullptr && task != CurrentTask();
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
    if (task == nullptr || task->state != TaskState::Complete || task == CurrentTask())
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
    if (!initialized || CurrentTask() == &IdleTask())
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
    CurrentTask()->state = TaskState::Blocked;
    CurrentTask()->wait_queue = &queue;
    CurrentTask()->wake_tick = deadline_tick;
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
    if (!initialized || CurrentTask() == &IdleTask())
    {
        return false;
    }

    const uint64_t flags = scheduler_lock.LockIrqSave();
    if (Time::TickCount() < tick)
    {
        CurrentTask()->state = TaskState::Sleeping;
        CurrentTask()->wake_tick = tick;
        Schedule();
    }
    scheduler_lock.UnlockIrqRestore(flags);
    return true;
}

void ActivateCurrentAddressSpace()
{
    ActivateTaskAddressSpace(CurrentTask());
}

void* CurrentUserContext()
{
    return CurrentTask() == nullptr ? nullptr : CurrentTask()->user_context;
}

void* SetCurrentUserContext(void* context)
{
    if (CurrentTask() == nullptr)
    {
        return nullptr;
    }
    const uint64_t flags = Arch::Interrupts::SaveAndDisable();
    void* previous_context = CurrentTask()->user_context;
    CurrentTask()->user_context = context;
    Arch::Interrupts::Restore(flags);
    return previous_context;
}

uint64_t CurrentKernelStackTop()
{
    if (CurrentTask() == nullptr || CurrentTask()->stack == nullptr)
    {
        return 0;
    }
    uintptr_t stack_top = reinterpret_cast<uintptr_t>(CurrentTask()->stack + TaskStackSize);
    stack_top &= ~static_cast<uintptr_t>(0xF);
    return stack_top;
}

} // namespace FeatherOS::Scheduler
