#include "scheduler.h"

#include "kernel/arch/interrupts.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/panic.h"
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
    uint64_t interrupt_state;
    void* user_context;
    alignas(16) uint8_t stack[TaskStackSize];
};

Task tasks[MaximumTasks];
int32_t current_task_index = -1;
uint64_t kernel_stack_pointer = 0;
uint64_t kernel_interrupt_state = 0;
TaskId next_task_id = 1;
bool initialized = false;
volatile bool preemption_enabled = false;

extern "C" void FeatherOS_Arch_SwitchContext(uint64_t* old_stack_pointer,
                                             uint64_t new_stack_pointer,
                                             uint64_t new_interrupt_state);

[[noreturn]] void TaskReturnFailure()
{
    FeatherOS::Panic("Cooperative task returned through an invalid stack frame");
}

Task* FindTask(TaskId task_id, size_t* task_index = nullptr)
{
    if (task_id == 0)
    {
        return nullptr;
    }
    for (size_t index = 0; index < MaximumTasks; ++index)
    {
        if (tasks[index].id == task_id && tasks[index].state != TaskState::Empty)
        {
            if (task_index != nullptr)
            {
                *task_index = index;
            }
            return &tasks[index];
        }
    }
    return nullptr;
}

void TaskBootstrap()
{
    if (current_task_index < 0 || tasks[current_task_index].entry == nullptr)
    {
        FeatherOS::Panic("Cooperative task bootstrap has no current task");
    }

    Task& task = tasks[current_task_index];
    task.exit_status = task.entry(task.context);
    task.state = TaskState::Complete;
    Yield();
    TaskReturnFailure();
}

bool FindReadyTask(size_t* task_index)
{
    if (task_index == nullptr)
    {
        return false;
    }
    const size_t start =
        current_task_index < 0 ? 0 : (static_cast<size_t>(current_task_index) + 1) % MaximumTasks;
    for (size_t offset = 0; offset < MaximumTasks; ++offset)
    {
        const size_t candidate = (start + offset) % MaximumTasks;
        if (static_cast<int32_t>(candidate) != current_task_index &&
            tasks[candidate].state == TaskState::Ready)
        {
            *task_index = candidate;
            return true;
        }
    }
    return false;
}

void ActivateTaskAddressSpace(int32_t task_index)
{
    if (task_index < 0)
    {
        Memory::ActivateKernelAddressSpace();
        return;
    }
    auto* user_context = static_cast<UserExecutionContext*>(tasks[task_index].user_context);
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

} // namespace

bool Initialize()
{
    for (Task& task : tasks)
    {
        task.stack_pointer = 0;
        task.entry = nullptr;
        task.context = nullptr;
        task.id = 0;
        task.state = TaskState::Empty;
        task.exit_status = 0;
        task.interrupt_state = 0;
        task.user_context = nullptr;
    }
    current_task_index = -1;
    kernel_stack_pointer = 0;
    kernel_interrupt_state = 0;
    next_task_id = 1;
    preemption_enabled = false;
    initialized = true;
    return true;
}

bool CreateTask(TaskEntry entry, void* context, TaskId* task_id)
{
    if (!initialized || entry == nullptr || task_id == nullptr || next_task_id == 0)
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
    task.entry = entry;
    task.context = context;
    task.id = next_task_id++;
    task.state = TaskState::Ready;
    task.exit_status = 0;
    task.user_context = nullptr;
#if defined(ARCH_x86_64)
    task.interrupt_state = 0x202;
#elif defined(ARCH_aarch64)
    task.interrupt_state = 0;
#endif
    uintptr_t stack_top = reinterpret_cast<uintptr_t>(task.stack + sizeof(task.stack));
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

    *task_id = task.id;
    return true;
}

bool Yield()
{
    if (!initialized)
    {
        return false;
    }

    const uint64_t interrupt_state = Arch::Interrupts::SaveAndDisable();
    const int32_t previous_task_index = current_task_index;
    uint64_t* old_stack_pointer =
        previous_task_index < 0 ? &kernel_stack_pointer : &tasks[previous_task_index].stack_pointer;
    if (previous_task_index >= 0 && tasks[previous_task_index].state == TaskState::Running)
    {
        tasks[previous_task_index].state = TaskState::Ready;
        tasks[previous_task_index].interrupt_state = interrupt_state;
    }
    else if (previous_task_index < 0)
    {
        kernel_interrupt_state = interrupt_state;
    }

    size_t next_index;
    if (!FindReadyTask(&next_index))
    {
        if (previous_task_index < 0)
        {
            Arch::Interrupts::Restore(interrupt_state);
            return false;
        }
        if (tasks[previous_task_index].state == TaskState::Ready)
        {
            tasks[previous_task_index].state = TaskState::Running;
            Arch::Interrupts::Restore(interrupt_state);
            return false;
        }
        current_task_index = -1;
        ActivateTaskAddressSpace(current_task_index);
        FeatherOS_Arch_SwitchContext(old_stack_pointer, kernel_stack_pointer,
                                     kernel_interrupt_state);
        return true;
    }

    current_task_index = static_cast<int32_t>(next_index);
    tasks[next_index].state = TaskState::Running;
    ActivateTaskAddressSpace(current_task_index);
    FeatherOS_Arch_SwitchContext(old_stack_pointer, tasks[next_index].stack_pointer,
                                 tasks[next_index].interrupt_state);
    return true;
}

void EnablePreemption()
{
    preemption_enabled = true;
}

void DisablePreemption()
{
    preemption_enabled = false;
}

void OnTimerTick()
{
    if (preemption_enabled && current_task_index >= 0)
    {
        Yield();
    }
}

bool Wait(TaskId task_id, uint64_t* exit_status)
{
    size_t task_index;
    Task* task = FindTask(task_id, &task_index);
    if (task == nullptr || static_cast<int32_t>(task_index) == current_task_index)
    {
        return false;
    }

    while (task->state != TaskState::Complete)
    {
        if (!Yield())
        {
            return false;
        }
        task = FindTask(task_id);
        if (task == nullptr)
        {
            return false;
        }
    }
    if (exit_status != nullptr)
    {
        *exit_status = task->exit_status;
    }
    return true;
}

bool ReapTask(TaskId task_id)
{
    size_t task_index;
    Task* task = FindTask(task_id, &task_index);
    if (task == nullptr || task->state != TaskState::Complete ||
        static_cast<int32_t>(task_index) == current_task_index)
    {
        return false;
    }

    task->stack_pointer = 0;
    task->entry = nullptr;
    task->context = nullptr;
    task->id = 0;
    task->exit_status = 0;
    task->state = TaskState::Empty;
    task->user_context = nullptr;
    return true;
}

TaskState GetState(TaskId task_id)
{
    const Task* task = FindTask(task_id);
    return task == nullptr ? TaskState::Empty : task->state;
}

void ActivateCurrentAddressSpace()
{
    ActivateTaskAddressSpace(current_task_index);
}

void* CurrentUserContext()
{
    return current_task_index < 0 ? nullptr : tasks[current_task_index].user_context;
}

void* SetCurrentUserContext(void* context)
{
    if (current_task_index < 0)
    {
        return nullptr;
    }
    const uint64_t interrupt_state = Arch::Interrupts::SaveAndDisable();
    void* previous_context = tasks[current_task_index].user_context;
    tasks[current_task_index].user_context = context;
    Arch::Interrupts::Restore(interrupt_state);
    return previous_context;
}

uint64_t CurrentKernelStackTop()
{
    if (current_task_index < 0)
    {
        return 0;
    }
    const Task& task = tasks[current_task_index];
    uintptr_t stack_top = reinterpret_cast<uintptr_t>(task.stack + sizeof(task.stack));
    stack_top &= ~static_cast<uintptr_t>(0xF);
    return stack_top;
}

} // namespace FeatherOS::Scheduler