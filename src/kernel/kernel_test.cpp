#include "kernel_test.h"

#include "fs/partition.h"
#include "kernel/arch/cpu_features.h"
#include "kernel/arch/serial.h"
#include "kernel/console.h"
#include "kernel/cpu.h"
#include "kernel/exception.h"
#include "kernel/filesystem/drivers.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/kernel_stack.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/network/interface.h"
#include "kernel/network/neighbor_discovery.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/scheduler.h"
#include "kernel/storage/block_device.h"
#include "kernel/storage/core_block_device.h"
#include "kernel/string/format.h"
#include "kernel/string/memory_functions.h"
#include "kernel/terminal/terminal.h"
#include "kernel/time/timer.h"

#if defined(ARCH_x86_64)
#include "kernel/arch/x86_64/interrupts.h"
#elif defined(ARCH_aarch64)
#include "kernel/arch/aarch64/interrupts.h"
#endif

#include <atomic>
#include <stdint.h>

extern "C" void FeatherOSMemoryTestWriteProtected(volatile uint8_t* address, uint8_t value);
extern "C" void FeatherOSMemoryTestReadUnmapped(volatile uint8_t* address);
extern "C" void FeatherOSMemoryTestExecuteNonExecutable(void* address);
extern "C" uint8_t FeatherOSMemoryTestWriteFault[];
extern "C" uint8_t FeatherOSMemoryTestWriteResume[];
extern "C" uint8_t FeatherOSMemoryTestReadFault[];
extern "C" uint8_t FeatherOSMemoryTestReadResume[];
extern "C" uint8_t FeatherOSMemoryTestExecuteResume[];

#if defined(ARCH_x86_64)
asm(".section .text\n"
    ".global FeatherOSMemoryTestWriteProtected\n"
    ".type FeatherOSMemoryTestWriteProtected,@function\n"
    "FeatherOSMemoryTestWriteProtected:\n"
    ".global FeatherOSMemoryTestWriteFault\n"
    "FeatherOSMemoryTestWriteFault:\n"
    "movb %sil, (%rdi)\n"
    ".global FeatherOSMemoryTestWriteResume\n"
    "FeatherOSMemoryTestWriteResume:\n"
    "ret\n"
    ".size FeatherOSMemoryTestWriteProtected, .-FeatherOSMemoryTestWriteProtected\n"
    ".global FeatherOSMemoryTestReadUnmapped\n"
    ".type FeatherOSMemoryTestReadUnmapped,@function\n"
    "FeatherOSMemoryTestReadUnmapped:\n"
    ".global FeatherOSMemoryTestReadFault\n"
    "FeatherOSMemoryTestReadFault:\n"
    "movb (%rdi), %al\n"
    ".global FeatherOSMemoryTestReadResume\n"
    "FeatherOSMemoryTestReadResume:\n"
    "ret\n"
    ".size FeatherOSMemoryTestReadUnmapped, .-FeatherOSMemoryTestReadUnmapped\n");
asm(".section .text\n"
    ".global FeatherOSMemoryTestExecuteNonExecutable\n"
    ".type FeatherOSMemoryTestExecuteNonExecutable,@function\n"
    "FeatherOSMemoryTestExecuteNonExecutable:\n"
    "call *%rdi\n"
    ".global FeatherOSMemoryTestExecuteResume\n"
    "FeatherOSMemoryTestExecuteResume:\n"
    "addq $8, %rsp\n"
    "ret\n"
    ".size FeatherOSMemoryTestExecuteNonExecutable, .-FeatherOSMemoryTestExecuteNonExecutable\n");
#elif defined(ARCH_aarch64)
asm(".section .text\n"
    ".global FeatherOSMemoryTestWriteProtected\n"
    ".type FeatherOSMemoryTestWriteProtected,%function\n"
    "FeatherOSMemoryTestWriteProtected:\n"
    ".global FeatherOSMemoryTestWriteFault\n"
    "FeatherOSMemoryTestWriteFault:\n"
    "strb w1, [x0]\n"
    ".global FeatherOSMemoryTestWriteResume\n"
    "FeatherOSMemoryTestWriteResume:\n"
    "ret\n"
    ".size FeatherOSMemoryTestWriteProtected, .-FeatherOSMemoryTestWriteProtected\n"
    ".global FeatherOSMemoryTestReadUnmapped\n"
    ".type FeatherOSMemoryTestReadUnmapped,%function\n"
    "FeatherOSMemoryTestReadUnmapped:\n"
    ".global FeatherOSMemoryTestReadFault\n"
    "FeatherOSMemoryTestReadFault:\n"
    "ldrb w2, [x0]\n"
    ".global FeatherOSMemoryTestReadResume\n"
    "FeatherOSMemoryTestReadResume:\n"
    "ret\n"
    ".size FeatherOSMemoryTestReadUnmapped, .-FeatherOSMemoryTestReadUnmapped\n");
asm(".section .text\n"
    ".global FeatherOSMemoryTestExecuteNonExecutable\n"
    ".type FeatherOSMemoryTestExecuteNonExecutable,%function\n"
    "FeatherOSMemoryTestExecuteNonExecutable:\n"
    "stp x29, x30, [sp, #-16]!\n"
    "blr x0\n"
    ".global FeatherOSMemoryTestExecuteResume\n"
    "FeatherOSMemoryTestExecuteResume:\n"
    "ldp x29, x30, [sp], #16\n"
    "ret\n"
    ".size FeatherOSMemoryTestExecuteNonExecutable, .-FeatherOSMemoryTestExecuteNonExecutable\n");
#endif

namespace
{

enum class ExpectedMemoryFault : uint8_t
{
    None,
    WriteToReadOnly,
    ReadFromUnmapped,
    ExecuteFromNonExecutable,
};

constexpr uint64_t TestVirtualAddress = 0xFFFFFE0000000000;
ExpectedMemoryFault expected_memory_fault = ExpectedMemoryFault::None;
uint64_t expected_fault_address = 0;
uint64_t recovered_memory_fault_count = 0;

bool RecoverExpectedMemoryFault(FeatherOS::ExceptionReport* report)
{
    if (report == nullptr || report->fault_address != expected_fault_address)
    {
        return false;
    }

    if (expected_memory_fault == ExpectedMemoryFault::WriteToReadOnly &&
        report->instruction_pointer == reinterpret_cast<uint64_t>(FeatherOSMemoryTestWriteFault))
    {
#if defined(ARCH_x86_64)
        if (report->architecture != FeatherOS::ExceptionArchitecture::X86_64 ||
            report->vector != 14 || (report->error_code & 0x3) != 0x3)
        {
            return false;
        }
        report->instruction_pointer = reinterpret_cast<uint64_t>(FeatherOSMemoryTestWriteResume);
#elif defined(ARCH_aarch64)
        if (report->architecture != FeatherOS::ExceptionArchitecture::AArch64 ||
            report->vector != 0x25 || (report->syndrome & 0x3F) != 0x0F ||
            (report->syndrome & (1U << 6)) == 0)
        {
            return false;
        }
        report->instruction_pointer = reinterpret_cast<uint64_t>(FeatherOSMemoryTestWriteResume);
#endif
    }
    else if (expected_memory_fault == ExpectedMemoryFault::ReadFromUnmapped &&
             report->instruction_pointer ==
                 reinterpret_cast<uint64_t>(FeatherOSMemoryTestReadFault))
    {
#if defined(ARCH_x86_64)
        if (report->architecture != FeatherOS::ExceptionArchitecture::X86_64 ||
            report->vector != 14 || (report->error_code & 0x3) != 0)
        {
            return false;
        }
        report->instruction_pointer = reinterpret_cast<uint64_t>(FeatherOSMemoryTestReadResume);
#elif defined(ARCH_aarch64)
        if (report->architecture != FeatherOS::ExceptionArchitecture::AArch64 ||
            report->vector != 0x25 || (report->syndrome & 0x3F) != 0x07 ||
            (report->syndrome & (1U << 6)) != 0)
        {
            return false;
        }
        report->instruction_pointer = reinterpret_cast<uint64_t>(FeatherOSMemoryTestReadResume);
#endif
    }
    else if (expected_memory_fault == ExpectedMemoryFault::ExecuteFromNonExecutable &&
             report->instruction_pointer == TestVirtualAddress)
    {
#if defined(ARCH_x86_64)
        if (report->architecture != FeatherOS::ExceptionArchitecture::X86_64 ||
            report->vector != 14 || (report->error_code & 0x13) != 0x11)
        {
            return false;
        }
        report->instruction_pointer = reinterpret_cast<uint64_t>(FeatherOSMemoryTestExecuteResume);
#elif defined(ARCH_aarch64)
        if (report->architecture != FeatherOS::ExceptionArchitecture::AArch64 ||
            report->vector != 0x21 || (report->syndrome & 0x3F) != 0x0F)
        {
            return false;
        }
        report->instruction_pointer = reinterpret_cast<uint64_t>(FeatherOSMemoryTestExecuteResume);
#endif
    }
    else
    {
        return false;
    }

    ++recovered_memory_fault_count;
    return true;
}

} // namespace

#if defined(ARCH_aarch64)
namespace
{

volatile uint64_t gic_interrupt_count = 0;

void OnGicInterrupt()
{
    gic_interrupt_count = gic_interrupt_count + 1;
}

} // namespace
#elif defined(ARCH_x86_64)
namespace
{

volatile uint64_t pit_interrupt_count = 0;

void OnPitInterrupt()
{
    pit_interrupt_count = pit_interrupt_count + 1;
}

} // namespace
#endif

namespace FeatherOS::KernelTest
{
namespace
{

void RunTimerTests(const void* device_tree)
{
    if (!Time::InitializeTimer(100, device_tree))
    {
        Panic("Periodic timer initialization failed");
    }
    uint64_t previous_tick = Time::TickCount();
    const uint64_t start_tick = previous_tick;
    for (uint32_t tick = 0; tick < 5; ++tick)
    {
        Time::WaitForNextTick();
        const uint64_t current_tick = Time::TickCount();
        if (current_tick <= previous_tick)
        {
            Panic("Timer ticks did not increase");
        }
        previous_tick = current_tick;
    }
    Time::StopTimer();
    if (previous_tick - start_tick < 5)
    {
        Panic("Timer lost periodic ticks");
    }
    Serial::Write("KERN/TIMER: periodic ticks passed\n");
}

} // namespace
} // namespace FeatherOS::KernelTest

namespace FeatherOS::KernelTest
{

namespace
{

uint32_t scheduler_sequence = 0;
volatile uint64_t preemption_progress[2] = {};
constinit Sync::SpinLock blocking_test_lock;
constinit Scheduler::WaitQueue blocking_test_queue;
constinit Scheduler::WaitQueue timeout_test_queue;
bool blocking_test_signaled GUARDED_BY(blocking_test_lock) = false;

uint64_t FirstCooperativeTask(void*)
{
    scheduler_sequence = scheduler_sequence * 10 + 1;
    if (!Scheduler::Yield())
    {
        Panic("First cooperative task did not yield");
    }
    scheduler_sequence = scheduler_sequence * 10 + 3;
    return 17;
}

uint64_t SecondCooperativeTask(void*)
{
    scheduler_sequence = scheduler_sequence * 10 + 2;
    if (!Scheduler::Yield())
    {
        Panic("Second cooperative task did not yield");
    }
    scheduler_sequence = scheduler_sequence * 10 + 4;
    return 29;
}

uint64_t ReusedCooperativeTask(void*)
{
    scheduler_sequence = scheduler_sequence * 10 + 5;
    return 41;
}

uint64_t FirstPreemptedTask(void*)
{
    while (preemption_progress[1] == 0)
    {
        preemption_progress[0] = preemption_progress[0] + 1;
    }
    return 53;
}

uint64_t SecondPreemptedTask(void*)
{
    do
    {
        preemption_progress[1] = preemption_progress[1] + 1;
    } while (preemption_progress[0] == 0);
    return 67;
}

uint64_t BlockedWaiterTask(void*)
{
    const uint64_t flags = blocking_test_lock.LockIrqSave();
    while (!blocking_test_signaled)
    {
        Scheduler::WaitOn(blocking_test_queue, blocking_test_lock);
    }
    blocking_test_lock.UnlockIrqRestore(flags);
    scheduler_sequence = scheduler_sequence * 10 + 7;
    return 71;
}

uint64_t SignalingTask(void*)
{
    scheduler_sequence = scheduler_sequence * 10 + 6;
    const uint64_t flags = blocking_test_lock.LockIrqSave();
    blocking_test_signaled = true;
    Scheduler::WakeAll(blocking_test_queue);
    blocking_test_lock.UnlockIrqRestore(flags);
    return 83;
}

uint64_t SleepingTask(void*)
{
    const uint64_t start = Time::TickCount();
    if (!Scheduler::SleepUntil(start + 5))
    {
        return UINT64_MAX;
    }
    return Time::TickCount() - start;
}

uint64_t TimedWaiterTask(void*)
{
    // Nothing wakes this queue; only the deadline ends the wait.
    const uint64_t flags = blocking_test_lock.LockIrqSave();
    const uint64_t start = Time::TickCount();
    Scheduler::WaitOn(timeout_test_queue, blocking_test_lock, start + 3);
    const uint64_t elapsed = Time::TickCount() - start;
    blocking_test_lock.UnlockIrqRestore(flags);
    return elapsed;
}

bool RunAndReap(Scheduler::TaskEntry entry, uint64_t* exit_status)
{
    Scheduler::TaskId task;
    return Scheduler::CreateTask(entry, nullptr, &task) && Scheduler::Wait(task, exit_status) &&
           Scheduler::ReapTask(task);
}

void RunBlockingTests(const void* platform_data)
{
    Scheduler::TaskId waiter_task;
    Scheduler::TaskId signaling_task;
    uint64_t waiter_status = 0;
    uint64_t signaling_status = 0;
    scheduler_sequence = 0;
    {
        Sync::SpinLockGuard guard(blocking_test_lock);
        blocking_test_signaled = false;
    }
    if (!Scheduler::CreateTask(BlockedWaiterTask, nullptr, &waiter_task) ||
        !Scheduler::CreateTask(SignalingTask, nullptr, &signaling_task) ||
        !Scheduler::Wait(waiter_task, &waiter_status) ||
        !Scheduler::Wait(signaling_task, &signaling_status) || !Scheduler::ReapTask(waiter_task) ||
        !Scheduler::ReapTask(signaling_task) || scheduler_sequence != 67 || waiter_status != 71 ||
        signaling_status != 83)
    {
        Panic("Wait-queue blocking and wakeup test failed");
    }

    // Exercises the idle task: nothing is ready until a tick.
    if (!Time::InitializeTimer(100, platform_data))
    {
        Panic("Could not initialize timer for scheduler sleep test");
    }
    uint64_t slept_ticks = 0;
    uint64_t waited_ticks = 0;
    const bool sleep_finished = RunAndReap(SleepingTask, &slept_ticks);
    const bool timed_wait_finished = RunAndReap(TimedWaiterTask, &waited_ticks);
    Time::StopTimer();
    if (!sleep_finished || slept_ticks < 5 || slept_ticks > 50 || !timed_wait_finished ||
        waited_ticks < 3 || waited_ticks > 50)
    {
        Panic("Scheduler sleep or wait timeout test failed");
    }
}

uint64_t PriorityTask(void* context)
{
    scheduler_sequence =
        scheduler_sequence * 10 + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context));
    return 0;
}

void RunPriorityTests()
{
    // Only the boot CPU schedules here, so the order is deterministic.
    constexpr Scheduler::TaskPriority Priorities[] = {
        Scheduler::TaskPriority::Low,
        Scheduler::TaskPriority::Normal,
        Scheduler::TaskPriority::High,
    };
    Scheduler::TaskId task_ids[3];
    scheduler_sequence = 0;
    for (uint64_t index = 0; index < 3; ++index)
    {
        if (!Scheduler::CreateTask(PriorityTask, reinterpret_cast<void*>(index + 1),
                                   &task_ids[index]) ||
            !Scheduler::SetPriority(task_ids[index], Priorities[index]))
        {
            Panic("Could not create priority test task");
        }
    }
    for (const Scheduler::TaskId task_id : task_ids)
    {
        if (!Scheduler::Wait(task_id, nullptr) || !Scheduler::ReapTask(task_id))
        {
            Panic("Priority test task did not finish");
        }
    }
    if (scheduler_sequence != 321)
    {
        Panic("Higher-priority tasks did not run first");
    }
}

uint64_t ScalingTask(void* context)
{
    Scheduler::Yield();
    return reinterpret_cast<uint64_t>(context);
}

void RunTaskScalingTests()
{
    constexpr uint64_t TaskCount = 12;
    constexpr uint32_t Rounds = 3;
    uint64_t free_frames_after_first_round = 0;
    for (uint32_t round = 0; round < Rounds; ++round)
    {
        Scheduler::TaskId task_ids[TaskCount];
        for (uint64_t index = 0; index < TaskCount; ++index)
        {
            if (!Scheduler::CreateTask(ScalingTask, reinterpret_cast<void*>(index),
                                       &task_ids[index]))
            {
                Panic("Could not create more than the old fixed number of tasks");
            }
        }
        for (uint64_t index = 0; index < TaskCount; ++index)
        {
            uint64_t exit_status;
            if (!Scheduler::Wait(task_ids[index], &exit_status) || exit_status != index ||
                !Scheduler::ReapTask(task_ids[index]))
            {
                Panic("Dynamically allocated task did not finish");
            }
        }
        // Later rounds reuse the pooled stacks and freed heap blocks, so no frames go missing.
        Memory::PageAllocatorStats stats;
        if (!Memory::GetPageAllocatorStats(&stats))
        {
            Panic("Page allocator stats unavailable");
        }
        if (round == 0)
        {
            free_frames_after_first_round = stats.free_frames;
        }
        else if (stats.free_frames != free_frames_after_first_round)
        {
            Panic("Creating and reaping tasks leaked page frames");
        }
    }
}

void RunSchedulerTests(const void* platform_data)
{
    Scheduler::TaskId first_task;
    Scheduler::TaskId second_task;
    Scheduler::TaskId reused_task;
    uint64_t first_exit_status;
    uint64_t second_exit_status;
    uint64_t reused_exit_status;
    scheduler_sequence = 0;
    if (!Scheduler::Initialize() ||
        !Scheduler::CreateTask(FirstCooperativeTask, nullptr, &first_task) ||
        !Scheduler::CreateTask(SecondCooperativeTask, nullptr, &second_task) ||
        Scheduler::Wait(0, nullptr) || Scheduler::ReapTask(first_task) ||
        !Scheduler::Wait(first_task, &first_exit_status) ||
        !Scheduler::Wait(second_task, &second_exit_status))
    {
        Panic("Cooperative scheduler setup or dispatch failed");
    }
    if (scheduler_sequence != 1234 || first_exit_status != 17 || second_exit_status != 29 ||
        Scheduler::GetState(first_task) != Scheduler::TaskState::Complete ||
        Scheduler::GetState(second_task) != Scheduler::TaskState::Complete ||
        !Scheduler::ReapTask(first_task) || !Scheduler::ReapTask(second_task) ||
        Scheduler::GetState(first_task) != Scheduler::TaskState::Empty ||
        Scheduler::ReapTask(first_task) ||
        !Scheduler::CreateTask(ReusedCooperativeTask, nullptr, &reused_task) ||
        reused_task == first_task || !Scheduler::Wait(reused_task, &reused_exit_status) ||
        reused_exit_status != 41 || scheduler_sequence != 12345 ||
        !Scheduler::ReapTask(reused_task) || Scheduler::Yield())
    {
        Panic("Cooperative wait, exit, or reap behavior is incorrect");
    }

    Scheduler::TaskId first_preempted_task;
    Scheduler::TaskId second_preempted_task;
    uint64_t first_preempted_status;
    uint64_t second_preempted_status;
    preemption_progress[0] = 0;
    preemption_progress[1] = 0;
    if (!Time::InitializeTimer(100, platform_data))
    {
        Panic("Could not initialize timer for scheduler preemption test");
    }
    Scheduler::EnablePreemption();
    const bool preemption_tasks_created =
        Scheduler::CreateTask(FirstPreemptedTask, nullptr, &first_preempted_task) &&
        Scheduler::CreateTask(SecondPreemptedTask, nullptr, &second_preempted_task);
    const bool preemption_tasks_finished =
        preemption_tasks_created &&
        Scheduler::Wait(first_preempted_task, &first_preempted_status) &&
        Scheduler::Wait(second_preempted_task, &second_preempted_status);
    Scheduler::DisablePreemption();
    Time::StopTimer();
    if (!preemption_tasks_finished || first_preempted_status != 53 ||
        second_preempted_status != 67 || preemption_progress[0] == 0 ||
        preemption_progress[1] == 0 || !Scheduler::ReapTask(first_preempted_task) ||
        !Scheduler::ReapTask(second_preempted_task))
    {
        Panic("Timer-driven task preemption test failed");
    }
    RunBlockingTests(platform_data);
    RunTaskScalingTests();
    RunPriorityTests();
    Serial::Write(
        "KERN/SCHEDULER: preemption, wait, exit, reap, blocking, sleep, and priorities passed\n");
}

} // namespace

namespace
{

void RunMemoryTests()
{
    char formatted[64];
    const char expected_format[] = "heap=42, string=typed, enabled=true, bytes=1234567890123, {ok}";
    if (!String::FormatTo(formatted, sizeof(formatted),
                          "heap={}, string={}, enabled={}, bytes={}, {{ok}}", 42U, "typed", true,
                          static_cast<uint64_t>(1234567890123ULL)))
    {
        Panic("String format failed");
    }
    size_t formatted_index = 0;
    while (expected_format[formatted_index] != '\0' &&
           formatted[formatted_index] == expected_format[formatted_index])
    {
        ++formatted_index;
    }
    if (expected_format[formatted_index] != '\0' || formatted[formatted_index] != '\0')
    {
        Panic("String format test failed");
    }
    if (String::FormatTo(formatted, sizeof(formatted), "missing {}") ||
        String::FormatTo(formatted, sizeof(formatted), "{}", 1U, 2U))
    {
        Panic("String format argument-count test failed");
    }

    void* c_allocation = malloc(37);
    if (c_allocation == nullptr || reinterpret_cast<uintptr_t>(c_allocation) % 16 != 0)
    {
        Panic("malloc alignment/allocation test failed");
    }
    static_cast<uint8_t*>(c_allocation)[0] = 0x5A;
    if (static_cast<uint8_t*>(c_allocation)[0] != 0x5A || !Memory::Deallocate(c_allocation) ||
        Memory::Deallocate(c_allocation))
    {
        Panic("heap free/double-free test failed");
    }

    auto* zeroed = static_cast<uint32_t*>(calloc(8, sizeof(uint32_t)));
    if (zeroed == nullptr)
    {
        Panic("calloc allocation test failed");
    }
    for (size_t index = 0; index < 8; ++index)
    {
        if (zeroed[index] != 0)
        {
            Panic("calloc zeroing test failed");
        }
    }
    free(zeroed);
    if (calloc(SIZE_MAX, 2) != nullptr)
    {
        Panic("calloc overflow test failed");
    }

    auto* scalar = new uint64_t;
    *scalar = 0x123456789ABCDEF0;
    if (reinterpret_cast<uintptr_t>(scalar) % 16 != 0 || *scalar != 0x123456789ABCDEF0)
    {
        Panic("C++ scalar new test failed");
    }
    delete scalar;

    constexpr size_t large_allocation_size = 3 * Memory::PageSize + 37;
    auto* large = new uint8_t[large_allocation_size];
    large[0] = 0xA5;
    large[large_allocation_size - 1] = 0x5A;
    if (large[0] != 0xA5 || large[large_allocation_size - 1] != 0x5A)
    {
        Panic("multi-page C++ array allocation test failed");
    }
    delete[] large;

    Memory::PageAllocatorStats initial_stats;
    if (!Memory::GetPageAllocatorStats(&initial_stats) || initial_stats.free_frames < 2)
    {
        Panic("Page-frame allocator has fewer than two free frames");
    }

    uint64_t first_frame;
    uint64_t second_frame;
    if (!Memory::AllocatePageFrame(&first_frame) || !Memory::AllocatePageFrame(&second_frame) ||
        first_frame == second_frame || first_frame % Memory::PageSize != 0 ||
        second_frame % Memory::PageSize != 0)
    {
        Panic("Page-frame allocation test failed");
    }

    Memory::PageAllocatorStats allocated_stats;
    if (!Memory::GetPageAllocatorStats(&allocated_stats) ||
        allocated_stats.free_frames + 2 != initial_stats.free_frames ||
        Memory::FreePageFrame(first_frame + 1) || !Memory::FreePageFrame(first_frame) ||
        !Memory::FreePageFrame(second_frame) || Memory::FreePageFrame(first_frame))
    {
        Panic("Page-frame free/accounting test failed");
    }
    uint64_t reused_frame;
    if (!Memory::AllocatePageFrame(&reused_frame) || reused_frame != first_frame ||
        !Memory::FreePageFrame(reused_frame))
    {
        Panic("Page-frame reuse test failed");
    }

    uint64_t mapped_frame;
    uint64_t adjacent_frame;
    if (!Memory::AllocatePageFrame(&mapped_frame) || !Memory::AllocatePageFrame(&adjacent_frame) ||
        Memory::MapPage(TestVirtualAddress + 1, mapped_frame,
                        Memory::PageFlags::Read | Memory::PageFlags::Write) ||
        !Memory::MapPage(TestVirtualAddress, mapped_frame,
                         Memory::PageFlags::Read | Memory::PageFlags::Write) ||
        !Memory::MapPage(TestVirtualAddress + Memory::PageSize, adjacent_frame,
                         Memory::PageFlags::Read | Memory::PageFlags::Write) ||
        Memory::MapPage(TestVirtualAddress, mapped_frame,
                        Memory::PageFlags::Read | Memory::PageFlags::Write) ||
        Memory::UnmapPage(TestVirtualAddress + 1))
    {
        Panic("Virtual mapping test setup failed");
    }
    auto* mapped_value = reinterpret_cast<volatile uint64_t*>(TestVirtualAddress);
    auto* adjacent_value =
        reinterpret_cast<volatile uint64_t*>(TestVirtualAddress + Memory::PageSize);
    *mapped_value = 0xFEEDC0DE5A17B10C;
    *adjacent_value = 0xABCD12345678EF90;
    if (*mapped_value != 0xFEEDC0DE5A17B10C || *adjacent_value != 0xABCD12345678EF90 ||
        !Memory::UnmapPage(TestVirtualAddress) ||
        !Memory::UnmapPage(TestVirtualAddress + Memory::PageSize) ||
        Memory::UnmapPage(TestVirtualAddress))
    {
        Panic("Virtual mapping test failed");
    }

    recovered_memory_fault_count = 0;
    expected_fault_address = TestVirtualAddress;
    expected_memory_fault = ExpectedMemoryFault::ReadFromUnmapped;
    SetTestExceptionHandler(RecoverExpectedMemoryFault);
    FeatherOSMemoryTestReadUnmapped(reinterpret_cast<volatile uint8_t*>(TestVirtualAddress));
    SetTestExceptionHandler(nullptr);
    if (recovered_memory_fault_count != 1)
    {
        Panic("TLB invalidation after unmap failed");
    }

    if (!Memory::MapPage(TestVirtualAddress, mapped_frame, Memory::PageFlags::Read))
    {
        Panic("Read-only mapping setup failed");
    }
    const volatile uint64_t readable_value = *mapped_value;
    (void)readable_value;
    expected_fault_address = TestVirtualAddress;
    expected_memory_fault = ExpectedMemoryFault::WriteToReadOnly;
    SetTestExceptionHandler(RecoverExpectedMemoryFault);
    FeatherOSMemoryTestWriteProtected(reinterpret_cast<volatile uint8_t*>(TestVirtualAddress),
                                      0x5A);
    SetTestExceptionHandler(nullptr);
    if (recovered_memory_fault_count != 2 || !Memory::UnmapPage(TestVirtualAddress))
    {
        Panic("Read-only page fault test failed");
    }

    expected_fault_address = TestVirtualAddress + 2 * Memory::PageSize;
    expected_memory_fault = ExpectedMemoryFault::ReadFromUnmapped;
    SetTestExceptionHandler(RecoverExpectedMemoryFault);
    FeatherOSMemoryTestReadUnmapped(reinterpret_cast<volatile uint8_t*>(expected_fault_address));
    SetTestExceptionHandler(nullptr);
    if (recovered_memory_fault_count != 3 || Memory::UnmapPage(expected_fault_address))
    {
        Panic("Unmapped page fault test failed");
    }

    if (!Memory::MapPage(TestVirtualAddress, mapped_frame,
                         Memory::PageFlags::Read | Memory::PageFlags::Write))
    {
        Panic("Executable probe page setup failed");
    }
#if defined(ARCH_x86_64)
    *reinterpret_cast<volatile uint8_t*>(TestVirtualAddress) = 0xC3;
#elif defined(ARCH_aarch64)
    *reinterpret_cast<volatile uint32_t*>(TestVirtualAddress) = 0xD65F03C0;
#endif
    if (!Memory::UnmapPage(TestVirtualAddress) ||
        !Memory::MapPage(TestVirtualAddress, mapped_frame,
                         Memory::PageFlags::Read | Memory::PageFlags::Write))
    {
        Panic("Non-executable probe mapping failed");
    }
    expected_fault_address = TestVirtualAddress;
    expected_memory_fault = ExpectedMemoryFault::ExecuteFromNonExecutable;
    SetTestExceptionHandler(RecoverExpectedMemoryFault);
    FeatherOSMemoryTestExecuteNonExecutable(reinterpret_cast<void*>(TestVirtualAddress));
    SetTestExceptionHandler(nullptr);
    if (recovered_memory_fault_count != 4 || !Memory::UnmapPage(TestVirtualAddress) ||
        !Memory::FreePageFrame(mapped_frame) || !Memory::FreePageFrame(adjacent_frame))
    {
        Panic("Non-executable page fault test failed");
    }

#if defined(ARCH_x86_64)
    constexpr uint64_t user_test_address = 0x00400000;
#elif defined(ARCH_aarch64)
    constexpr uint64_t user_test_address = 0x0000400000000000;
#endif
    Memory::AddressSpace user_address_space{};
    Memory::PageAllocatorStats frames_before_user_space;
    if (!Memory::GetPageAllocatorStats(&frames_before_user_space))
    {
        Panic("Could not read allocator stats before user address-space test");
    }
    uint64_t user_frame;
    if (!Memory::CreateAddressSpace(&user_address_space) ||
        !Memory::AllocatePageFrame(&user_frame) ||
        !Memory::MapPage(user_address_space, user_test_address, user_frame,
                         Memory::PageFlags::Read | Memory::PageFlags::Write |
                             Memory::PageFlags::User) ||
        !Memory::ActivateAddressSpace(user_address_space))
    {
        Panic("User address-space setup failed");
    }
    // User page tables share nothing with the kernel's low mappings, so a user page can use
    // the address where the kernel's early UART identity map sits.
    constexpr uint64_t low_device_address = 0x09000000;
    uint64_t low_frame;
    if (!Memory::AllocatePageFrame(&low_frame) ||
        !Memory::MapPage(user_address_space, low_device_address, low_frame,
                         Memory::PageFlags::Read | Memory::PageFlags::User))
    {
        Panic("User page at a kernel identity-mapped address was rejected");
    }
    auto* user_value = reinterpret_cast<volatile uint64_t*>(user_test_address);
    uint64_t observed_user_value;
    {
        const Arch::CpuFeatures::UserAccessWindow user_access;
        *user_value = 0xA11CE55A1234FEDC;
        observed_user_value = *user_value;
    }
    if (Memory::DestroyAddressSpace(&user_address_space))
    {
        Panic("Active user address space was destroyed");
    }
    Memory::ActivateKernelAddressSpace();
    if (observed_user_value != 0xA11CE55A1234FEDC ||
        !Memory::DestroyAddressSpace(&user_address_space))
    {
        Panic("User address-space mapping or destruction test failed");
    }
    Memory::PageAllocatorStats frames_after_user_space;
    if (!Memory::GetPageAllocatorStats(&frames_after_user_space) ||
        frames_after_user_space.free_frames != frames_before_user_space.free_frames)
    {
        Panic("Destroying a user address space leaked page frames");
    }

    uint8_t* kernel_stack = Memory::AllocateKernelStack();
    if (kernel_stack == nullptr)
    {
        Panic("Kernel stack allocation failed");
    }
    kernel_stack[0] = 0x5A;
    kernel_stack[Memory::KernelStackSize - 1] = 0xA5;
    expected_fault_address = reinterpret_cast<uint64_t>(kernel_stack) - 1;
    expected_memory_fault = ExpectedMemoryFault::ReadFromUnmapped;
    SetTestExceptionHandler(RecoverExpectedMemoryFault);
    FeatherOSMemoryTestReadUnmapped(reinterpret_cast<volatile uint8_t*>(expected_fault_address));
    SetTestExceptionHandler(nullptr);
    Memory::FreeKernelStack(kernel_stack);
    if (recovered_memory_fault_count != 5 || Memory::AllocateKernelStack() != kernel_stack)
    {
        Panic("Kernel stack guard page or reuse test failed");
    }
    Serial::Write("KERN/MEMORY: tests passed\n");
}

} // namespace

namespace
{

constexpr uint32_t SmpTestTasks = 3;
constexpr uint64_t SmpCounterIterations = 20000;
constexpr uint64_t SmpStartTimeoutTicks = 200;

constinit Sync::SpinLock smp_counter_lock;
uint64_t smp_counter GUARDED_BY(smp_counter_lock) = 0;
std::atomic<uint32_t> smp_started_tasks = 0;
std::atomic<uint32_t> smp_concurrent_tasks = 0;
std::atomic<uint32_t> smp_cpus_seen = 0;

uint64_t SmpTask(void*)
{
    smp_cpus_seen.fetch_or(1U << Cpu::CurrentIndex(), std::memory_order_relaxed);
    smp_started_tasks.fetch_add(1U, std::memory_order_acq_rel);
    // Preemption is off, so every task is running at once only if each has its own CPU.
    const uint64_t deadline = Time::TickCount() + SmpStartTimeoutTicks;
    while (smp_started_tasks.load(std::memory_order_acquire) < SmpTestTasks &&
           Time::TickCount() < deadline)
    {
        Sync::CpuRelax();
    }
    if (smp_started_tasks.load(std::memory_order_acquire) == SmpTestTasks)
    {
        smp_concurrent_tasks.fetch_add(1U, std::memory_order_relaxed);
    }
    for (uint64_t iteration = 0; iteration < SmpCounterIterations; ++iteration)
    {
        Sync::SpinLockGuard guard(smp_counter_lock);
        ++smp_counter;
    }
    // Stay on the CPU across two of its timer ticks so the scheduler records it as busy.
    const uint32_t cpu = Cpu::CurrentIndex();
    const uint64_t busy_until = Time::CpuTickCount(cpu) + 2;
    while (Time::CpuTickCount(cpu) < busy_until)
    {
        Sync::CpuRelax();
    }
    return 0;
}

} // namespace

void RunSmpTests()
{
    uint64_t busy_before[Cpu::MaximumCpus];
    for (uint32_t cpu = 0; cpu < Cpu::MaximumCpus; ++cpu)
    {
        busy_before[cpu] = Scheduler::BusyTickCount(cpu);
    }
    Scheduler::TaskId task_ids[SmpTestTasks];
    for (Scheduler::TaskId& task_id : task_ids)
    {
        if (!Scheduler::CreateTask(SmpTask, nullptr, &task_id))
        {
            Panic("Could not create SMP test task");
        }
    }
    for (const Scheduler::TaskId task_id : task_ids)
    {
        if (!Scheduler::Wait(task_id, nullptr) || !Scheduler::ReapTask(task_id))
        {
            Panic("SMP test task did not finish");
        }
    }
    uint32_t cpus_used = 0;
    uint32_t cpus_counted_busy = 0;
    for (uint32_t cpu = 0; cpu < Cpu::MaximumCpus; ++cpu)
    {
        if ((smp_cpus_seen.load(std::memory_order_relaxed) & (1U << cpu)) != 0)
        {
            ++cpus_used;
            cpus_counted_busy += Scheduler::BusyTickCount(cpu) > busy_before[cpu] ? 1 : 0;
        }
    }
    uint64_t counted;
    {
        Sync::SpinLockGuard guard(smp_counter_lock);
        counted = smp_counter;
    }
    if (smp_concurrent_tasks.load(std::memory_order_relaxed) != SmpTestTasks ||
        cpus_used != SmpTestTasks || counted != SmpTestTasks * SmpCounterIterations)
    {
        Panic("Tasks did not run concurrently on separate CPUs");
    }
    if (cpus_counted_busy != SmpTestTasks)
    {
        Panic("Busy CPUs were not counted");
    }
    Serial::Write("KERN/SMP: tasks ran concurrently on 3 CPUs; locked counter exact; busy CPUs "
                  "counted\n");
}

namespace
{

// The last sector before the root's partition, which nothing else uses.
constexpr uint64_t ScratchSector = 2047;
constexpr char BootCountMagic[] = "FOSBOOT";

uint8_t PatternByte(uint32_t index, uint64_t boot_count)
{
    return static_cast<uint8_t>(static_cast<uint64_t>(index) * 7 + boot_count);
}

// The disk's FAT16 partition, its second, which nothing mounts at boot. The host-side check in
// tools/smoke/cases/storage.py rebuilds the same text.
constexpr char FatPoint[] = "/fat";
constexpr char PersistentFile[] = "/fat/PERSIST.TXT";
constexpr char ScratchFile[] = "/fat/SCRATCH.TMP";
constexpr uint32_t PersistentLines = 200;

uint64_t FormatLine(char (&line)[48], uint64_t boot_count, uint32_t line_number)
{
    if (!String::FormatTo(line, sizeof(line), "FeatherOS boot {} line {}\n", boot_count,
                          line_number))
    {
        Panic("Storage test line formatting failed");
    }
    uint64_t length = 0;
    while (line[length] != '\0')
    {
        ++length;
    }
    return length;
}

void WriteLines(VFS::FileHandle* file, uint64_t boot_count, uint32_t line_count)
{
    for (uint32_t line_number = 0; line_number < line_count; ++line_number)
    {
        char line[48];
        const uint64_t length = FormatLine(line, boot_count, line_number);
        uint64_t written;
        if (!VFS::Write(file, line, length, &written) || written != length)
        {
            Panic("Storage test file write failed");
        }
    }
}

void CheckPersistentFile(uint64_t boot_count)
{
    VFS::FileHandle file;
    if (!VFS::Open(PersistentFile, &file))
    {
        Panic("Storage test file is missing");
    }
    for (uint32_t line_number = 0; line_number < PersistentLines; ++line_number)
    {
        char expected[48];
        const uint64_t length = FormatLine(expected, boot_count, line_number);
        char actual[48];
        uint64_t bytes_read;
        if (!VFS::Read(&file, actual, length, &bytes_read) || bytes_read != length)
        {
            Panic("Storage test file is too short");
        }
        for (uint64_t index = 0; index < length; ++index)
        {
            if (actual[index] != expected[index])
            {
                Panic("Storage test file has the wrong contents");
            }
        }
    }
    char extra;
    uint64_t bytes_read;
    if (!VFS::Read(&file, &extra, 1, &bytes_read) || bytes_read != 0)
    {
        Panic("Storage test file is too long");
    }
    VFS::Close(&file);
}

// 0 when the disk cannot say.
uint64_t FreeDiskBlocks()
{
    FeatherDisk::VolumeStatistics statistics = {};
    return Filesystem::Drivers::Disk().Statistics(&statistics) ? statistics.free_blocks : 0;
}

// Rewrites the previous boot's file in place, then checks that deleting a file returns its
// clusters.
void RunFileTests(uint64_t boot_count)
{
    VFS::FileHandle file;
    const bool exists = VFS::Open(PersistentFile, &file);
    if (exists != (boot_count > 1))
    {
        Panic("Storage test file state does not match the boot count");
    }
    if (exists)
    {
        VFS::Close(&file);
        CheckPersistentFile(boot_count - 1);
        if (!VFS::Open(PersistentFile, &file) || !VFS::Truncate(&file, 0))
        {
            Panic("Storage test file truncation failed");
        }
    }
    else if (!VFS::Create(PersistentFile, &file))
    {
        Panic("Storage test file creation failed");
    }
    WriteLines(&file, boot_count, PersistentLines);
    VFS::Close(&file);
    CheckPersistentFile(boot_count);

    const uint64_t free_clusters = FreeDiskBlocks();
    if (!VFS::Create(ScratchFile, &file))
    {
        Panic("Storage scratch file creation failed");
    }
    WriteLines(&file, boot_count, 100);
    VFS::Close(&file);
    if (FreeDiskBlocks() >= free_clusters || !VFS::Remove(ScratchFile) ||
        FreeDiskBlocks() != free_clusters)
    {
        Panic("Removing the storage scratch file did not free its clusters");
    }
}

void MountFat16()
{
    if (VFS::Mount("fat16", "vda1", FatPoint) != VFS::MountResult::NotFound ||
        VFS::Mount("ext2", "vda2", FatPoint) != VFS::MountResult::NotFound ||
        VFS::Mount("fat16", "vda2", FatPoint) != VFS::MountResult::Done)
    {
        Panic("Mounting the FAT16 partition failed");
    }
}

// For the files tools/create_test_disk.py puts on the disk.
bool ReadsAs(const char* path, const char* expected)
{
    VFS::FileHandle file;
    if (!VFS::Open(path, &file))
    {
        return false;
    }
    char contents[64];
    uint64_t length = 0;
    const bool read = VFS::Read(&file, contents, sizeof(contents), &length);
    VFS::Close(&file);
    uint64_t index = 0;
    while (read && index < length && expected[index] == contents[index])
    {
        ++index;
    }
    return read && index == length && expected[index] == '\0';
}

// Lines of "extN line NNNNN\n", past what direct blocks reach: the ext2 and ext3 partitions'
// big.txt.
bool BigFileMatches(const char* path, const char (&prefix)[11], uint32_t lines)
{
    constexpr uint64_t LineLength = 16;
    VFS::FileHandle file{};
    if (!VFS::Open(path, &file) || file.size != lines * LineLength)
    {
        VFS::Close(&file);
        return false;
    }
    bool matches = true;
    for (uint32_t line = 0; matches && line < lines; ++line)
    {
        char text[LineLength];
        uint64_t length = 0;
        matches = VFS::Read(&file, text, LineLength, &length) && length == LineLength;
        for (uint32_t index = 0; matches && index < sizeof(prefix) - 1; ++index)
        {
            matches = text[index] == prefix[index];
        }
        uint32_t number = line;
        for (uint32_t digit = 0; matches && digit < 5; ++digit)
        {
            matches = text[14 - digit] == static_cast<char>('0' + number % 10);
            number /= 10;
        }
        matches = matches && text[15] == '\n';
    }
    VFS::Close(&file);
    return matches;
}

// The file the read-write checks write: 1000 lines of "feather ext2 line NNNNN\n", past the
// 12 direct blocks of the test disk's 1 KiB blocks. tools/smoke/cases/storage.py checks it too.
constexpr uint32_t Ext2DataLines = 1000;
constexpr uint64_t Ext2DataLineLength = 24;
constexpr uint64_t Ext2DataSize = Ext2DataLines * Ext2DataLineLength;

struct Ext2Data
{
    char bytes[Ext2DataSize];
    char read_back[Ext2DataSize];
};

void FillExt2Data(char* bytes)
{
    constexpr char Prefix[] = "feather ext2 line ";
    for (uint32_t line = 0; line < Ext2DataLines; ++line)
    {
        char* text = bytes + line * Ext2DataLineLength;
        for (uint32_t index = 0; index < sizeof(Prefix) - 1; ++index)
        {
            text[index] = Prefix[index];
        }
        uint32_t number = line;
        for (uint32_t digit = 0; digit < 5; ++digit)
        {
            text[22 - digit] = static_cast<char>('0' + number % 10);
            number /= 10;
        }
        text[23] = '\n';
    }
}

bool WriteNew(const char* path, const char* data, uint64_t length)
{
    VFS::FileHandle file{};
    if (!VFS::Create(path, &file))
    {
        return false;
    }
    uint64_t written = 0;
    const bool done = VFS::Write(&file, data, length, &written) && written == length;
    VFS::Close(&file);
    return done;
}

bool HoldsExt2Data(const char* path, Ext2Data* data)
{
    VFS::FileHandle file{};
    if (!VFS::Open(path, &file))
    {
        return false;
    }
    uint64_t length = 0;
    const bool read = file.size == Ext2DataSize &&
                      VFS::Read(&file, data->read_back, Ext2DataSize, &length) &&
                      length == Ext2DataSize;
    VFS::Close(&file);
    for (uint64_t index = 0; read && index < Ext2DataSize; ++index)
    {
        if (data->read_back[index] != data->bytes[index])
        {
            return false;
        }
    }
    return read;
}

bool LinkCountIs(const char* path, uint32_t count)
{
    VFS::FileInfo info{};
    return VFS::Stat(path, &info) && info.link_count == count;
}

bool LinkTargetIs(const char* path, const char* expected)
{
    char target[32];
    uint32_t length = 0;
    if (!VFS::ReadLink(path, target, sizeof(target), &length) || length >= sizeof(target))
    {
        return false;
    }
    target[length] = '\0';
    uint32_t index = 0;
    while (expected[index] != '\0' && expected[index] == target[index])
    {
        ++index;
    }
    return expected[index] == '\0' && index == length;
}

struct VolumePath
{
    char text[64];
};

// root is "" for the root filesystem or the mount point of another volume.
VolumePath Join(const char* root, const char* tail)
{
    VolumePath path{};
    if (!String::FormatTo(path.text, sizeof(path.text), "{}{}", root, tail))
    {
        Panic("A storage test path is too long");
    }
    return path;
}

// The first boot builds /os on a volume; the second checks it and changes it again. After each
// boot the harness runs e2fsck -fn on the partitions and reads them with debugfs.
void WriteVolumeFirstBoot(const char* root, Ext2Data* data)
{
    constexpr char Short[] = "short\nand the rest is truncated away";
    VFS::FileHandle file{};
    if (!VFS::MakeDirectory(Join(root, "/os").text) ||
        !WriteNew(Join(root, "/os/data.txt").text, data->bytes, Ext2DataSize) ||
        !VFS::Link(Join(root, "/os/data.txt").text, Join(root, "/os/data-link.txt").text) ||
        !VFS::SymbolicLink(Join(root, "/os/to-data").text, "data.txt") ||
        !VFS::MakeDirectory(Join(root, "/os/sub").text) ||
        !WriteNew(Join(root, "/os/scratch.txt").text, "moved\n", 6) ||
        !VFS::Rename(Join(root, "/os/scratch.txt").text, Join(root, "/os/sub/moved.txt").text) ||
        !WriteNew(Join(root, "/os/short.txt").text, Short, sizeof(Short) - 1) ||
        !WriteNew(Join(root, "/os/gone.txt").text, "gone\n", 5) ||
        !VFS::Remove(Join(root, "/os/gone.txt").text) ||
        !VFS::MakeDirectory(Join(root, "/os/empty").text) ||
        !VFS::RemoveDirectory(Join(root, "/os/empty").text))
    {
        Panic("Writing a volume failed");
    }
    if (!VFS::Open(Join(root, "/os/short.txt").text, &file) || !VFS::Truncate(&file, 6))
    {
        Panic("Truncating a file failed");
    }
    VFS::Close(&file);
    if (VFS::MakeDirectory(Join(root, "/os").text) ||
        VFS::LastError() != User::SyscallErrorExists ||
        VFS::RemoveDirectory(Join(root, "/os/sub").text) ||
        VFS::LastError() != User::SyscallErrorNotEmpty ||
        VFS::Link(Join(root, "/os/sub").text, Join(root, "/os/sub-link").text) ||
        VFS::LastError() != User::SyscallErrorNotPermitted ||
        VFS::Rename(Join(root, "/os").text, Join(root, "/os/sub/inside").text) ||
        VFS::LastError() != User::SyscallErrorInvalidArgument ||
        VFS::Create(Join(root, "/missing/new.txt").text, &file) ||
        VFS::LastError() != User::SyscallErrorNotFound)
    {
        Panic("A change failed with the wrong error");
    }
}

void ChangeVolumeSecondBoot(const char* root, Ext2Data* data)
{
    VFS::FileInfo info{};
    if (!HoldsExt2Data(Join(root, "/os/data.txt").text, data) ||
        !HoldsExt2Data(Join(root, "/os/data-link.txt").text, data) ||
        !LinkCountIs(Join(root, "/os/data.txt").text, 2) ||
        !LinkTargetIs(Join(root, "/os/to-data").text, "data.txt") ||
        !ReadsAs(Join(root, "/os/sub/moved.txt").text, "moved\n") ||
        !ReadsAs(Join(root, "/os/short.txt").text, "short\n") ||
        VFS::IsDirectory(Join(root, "/os/empty").text) ||
        VFS::Stat(Join(root, "/os/gone.txt").text, &info))
    {
        Panic("A volume lost what the first boot wrote");
    }
    VFS::FileHandle file{};
    uint64_t written = 0;
    if (!VFS::Remove(Join(root, "/os/data.txt").text) ||
        !LinkCountIs(Join(root, "/os/data-link.txt").text, 1) ||
        !VFS::Remove(Join(root, "/os/to-data").text) ||
        !VFS::Rename(Join(root, "/os/sub").text, Join(root, "/moved-sub").text) ||
        !VFS::Open(Join(root, "/os/short.txt").text, &file) || !VFS::Seek(&file, file.size) ||
        !VFS::Write(&file, "more\n", 5, &written))
    {
        Panic("Changing a volume again failed");
    }
    VFS::Close(&file);
    if (!HoldsExt2Data(Join(root, "/os/data-link.txt").text, data) ||
        !ReadsAs(Join(root, "/moved-sub/moved.txt").text, "moved\n") ||
        !ReadsAs(Join(root, "/os/short.txt").text, "short\nmore\n"))
    {
        Panic("A volume read back different contents after the second changes");
    }
}

bool SameText(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

// The directory has count entries, name among them with the type bits given.
bool ListsWith(const char* path, uint32_t count, const char* name, uint32_t type)
{
    const Memory::HeapObject<Filesystem::DirectoryEntry> entry;
    bool found = false;
    uint32_t index = 0;
    for (; entry && VFS::ReadDirectory(path, index, &*entry); ++index)
    {
        found = found || (SameText(entry->name, name) && entry->type == type);
    }
    return found && index == count;
}

bool MountedAs(const char* point, const char* type, const char* source, bool read_only)
{
    const Memory::HeapObject<Procfs::MountStatus> status;
    for (uint32_t index = 0; status && VFS::DescribeMount(index, &*status); ++index)
    {
        if (SameText(status->point, point))
        {
            return SameText(status->type, type) && SameText(status->source, source) &&
                   status->read_only == read_only;
        }
    }
    return false;
}

// The ext4 partition's extents.bin (tools/create_test_disk.py): 2048 blocks of 1 KiB, each 32
// lines of "ext4 block BBBB line LL extents\n", except every fourth block, a hole of zeros. With
// 512 extents its tree is two levels deep.
constexpr uint32_t ExtentFileBlocks = 2048;
constexpr uint32_t ExtentBlockSize = 1024;
constexpr uint32_t ExtentLineLength = 32;

struct ExtentBlock
{
    char bytes[ExtentBlockSize];
};

void WriteDigits(char* text, uint32_t value, uint32_t count)
{
    for (uint32_t digit = count; digit-- > 0;)
    {
        text[digit] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
}

bool ExtentBlockMatches(const char* bytes, uint32_t block)
{
    if (block % 4 == 3)
    {
        for (uint32_t index = 0; index < ExtentBlockSize; ++index)
        {
            if (bytes[index] != '\0')
            {
                return false;
            }
        }
        return true;
    }
    char expected[] = "ext4 block 0000 line 00 extents\n";
    static_assert(sizeof(expected) == ExtentLineLength + 1);
    WriteDigits(expected + 11, block, 4);
    for (uint32_t line = 0; line < ExtentBlockSize / ExtentLineLength; ++line)
    {
        WriteDigits(expected + 21, line, 2);
        for (uint32_t index = 0; index < ExtentLineLength; ++index)
        {
            if (bytes[line * ExtentLineLength + index] != expected[index])
            {
                return false;
            }
        }
    }
    return true;
}

bool ExtentFileMatches(const char* path)
{
    const Memory::HeapObject<ExtentBlock> block;
    VFS::FileHandle file{};
    if (!block || !VFS::Open(path, &file))
    {
        return false;
    }
    bool matches = file.size == uint64_t{ExtentFileBlocks} * ExtentBlockSize;
    for (uint32_t number = 0; matches && number < ExtentFileBlocks; ++number)
    {
        uint64_t length = 0;
        matches = VFS::Read(&file, block->bytes, ExtentBlockSize, &length) &&
                  length == ExtentBlockSize && ExtentBlockMatches(block->bytes, number);
    }
    VFS::Close(&file);
    return matches;
}

// Nothing may change a read-only volume.
bool RefusesChanges(const char* point)
{
    char path[32];
    VFS::FileHandle file{};
    return String::FormatTo(path, sizeof(path), "{}/hello.txt", point) && VFS::IsReadOnly(path) &&
           !VFS::Remove(path) && VFS::LastError() == User::SyscallErrorReadOnly &&
           String::FormatTo(path, sizeof(path), "{}/new.txt", point) && !VFS::Create(path, &file) &&
           VFS::LastError() == User::SyscallErrorReadOnly &&
           String::FormatTo(path, sizeof(path), "{}/new", point) && !VFS::MakeDirectory(path);
}

// A read-only mount never replays a journal: what the first boot's writeback committed to the log
// is not visible yet, and what the first boot wrote and unmounted is.
void CheckReadOnlyBeforeReplay(const char* type, const char* source, const char* point)
{
    VFS::FileInfo info{};
    if (VFS::Mount(type, source, point, Filesystem::MountMode::ReadOnly) !=
            VFS::MountResult::Done ||
        !MountedAs(point, type, source, true) || !RefusesChanges(point) ||
        VFS::Stat(Join(point, "/periodic.txt").text, &info) ||
        !VFS::Stat(Join(point, "/os/data.txt").text, &info) ||
        VFS::Unmount(point) != VFS::MountResult::Done)
    {
        Panic("A read-only ext3 mount replayed its journal or took a change");
    }
}

// The first boot's shell session wrote /periodic.txt and stopped without unmounting, so the next
// writable mount replays the journal and finds it.
void CheckReplayedFile(const char* root, const char* text)
{
    if (!ReadsAs(Join(root, "/periodic.txt").text, text) ||
        !VFS::Rename(Join(root, "/periodic.txt").text, Join(root, "/os/replayed.txt").text))
    {
        Panic("An ext3 or ext4 mount did not replay the writeback's change");
    }
}

// The ext4 root's /samples (tools/create_test_disk.py): the ext4 test files, the depth-2 extent
// file among them.
bool SamplesMatch()
{
    return ReadsAs("/samples/hello.txt", "Hello from the ext4 partition.\n") &&
           ReadsAs("/samples/docs/nested/deep.txt", "nested ext4 file\n") &&
           ExtentFileMatches("/samples/extents.bin") &&
           LinkTargetIs("/samples/link", "docs/nested/deep.txt") &&
           ListsWith("/samples/docs", 1, "nested", User::ModeDirectory);
}

// The test disk's root holds BLOCK.TXT; the kernel itself reads no file from the root.
void CheckRootFile()
{
    constexpr char Contents[] = "FeatherOS root volume OK";
    VFS::FileInfo info{};
    if (!VFS::Stat("/BLOCK.TXT", &info) || info.size != sizeof(Contents) - 1 ||
        !ReadsAs("/BLOCK.TXT", Contents))
    {
        Panic("The root's /BLOCK.TXT read back different contents");
    }
}

// The root's journal gets the changes and is not closed here: the first boot's shell session stops
// the machine, so the second boot's root mount replays what only the writeback task committed.
void CheckRoot(uint64_t boot_count, Ext2Data* data)
{
    if (!SamplesMatch())
    {
        Panic("The ext4 root read back different contents");
    }
    if (boot_count == 1)
    {
        WriteVolumeFirstBoot("", data);
    }
    else if (boot_count == 2)
    {
        CheckReplayedFile("", "periodic ext4\n");
        ChangeVolumeSecondBoot("", data);
    }
    if (!VFS::SyncAll())
    {
        Panic("Syncing the written ext4 root failed");
    }
}

// The initramfs's default of /etc/<name> followed by suffix.
bool EtcFileIs(const char* name, const char* suffix)
{
    char path[sizeof("/etc/") + Initramfs::NameCapacity];
    if (!String::FormatTo(path, sizeof(path), "/etc/{}", name))
    {
        return false;
    }
    uint64_t suffix_length = 0;
    while (suffix[suffix_length] != '\0')
    {
        ++suffix_length;
    }
    Initramfs::File archive;
    VFS::FileHandle file{};
    if (!Initramfs::Open("etc", name, &archive) || !VFS::Open(path, &file))
    {
        return false;
    }
    bool same = file.size == archive.size + suffix_length;
    char chunk[64];
    uint64_t offset = 0;
    uint64_t count = 0;
    while (same && VFS::Read(&file, chunk, sizeof(chunk), &count) && count != 0)
    {
        for (uint64_t index = 0; index < count; ++index, ++offset)
        {
            const uint64_t past = offset < archive.size ? 0 : offset - archive.size;
            if (past >= suffix_length && offset >= archive.size)
            {
                same = false;
                break;
            }
            const char expected =
                offset < archive.size ? static_cast<char>(archive.data[offset]) : suffix[past];
            same = same && chunk[index] == expected;
        }
    }
    VFS::Close(&file);
    return same && offset == archive.size + suffix_length;
}

bool AppendTo(const char* path, const char* text)
{
    VFS::FileHandle file{};
    uint64_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    uint64_t written = 0;
    const bool done = VFS::Open(path, &file) && VFS::Seek(&file, file.size) &&
                      VFS::Write(&file, text, length, &written) && written == length;
    VFS::Close(&file);
    return done;
}

// /etc is the root's own directory, not the RAM copy; hosts, resolv.conf and init.conf keep an edit
// of the first boot, and a default deleted on the second comes back at the next UseRootEtc.
void CheckEtc(uint64_t boot_count)
{
    constexpr char FirstEdit[] = "# storage test boot 1\n";
    constexpr char SecondEdit[] = "# storage test boot 2\n";
    if (VFS::IsMountPoint("/etc") || !VFS::IsDirectory("/etc") || VFS::IsReadOnly("/etc/hosts"))
    {
        Panic("/etc is not the writable root's directory");
    }
    if (boot_count == 1)
    {
        if (!EtcFileIs("hosts", "") || !EtcFileIs("resolv.conf", "") ||
            !EtcFileIs("init.conf", "") || !AppendTo("/etc/hosts", FirstEdit) ||
            !AppendTo("/etc/resolv.conf", FirstEdit) || !AppendTo("/etc/init.conf", FirstEdit))
        {
            Panic("The root's /etc does not hold the defaults or took no edit");
        }
    }
    else if (boot_count == 2)
    {
        if (!EtcFileIs("hosts", FirstEdit) || !EtcFileIs("resolv.conf", FirstEdit) ||
            !EtcFileIs("init.conf", FirstEdit))
        {
            Panic("The root's /etc lost the first boot's edits");
        }
        VFS::FileInfo info;
        if (!VFS::Remove("/etc/resolv.conf") || VFS::Stat("/etc/resolv.conf", &info) ||
            !VFS::UseRootEtc() || !EtcFileIs("resolv.conf", "") || !EtcFileIs("hosts", FirstEdit) ||
            !AppendTo("/etc/hosts", SecondEdit) || !AppendTo("/etc/init.conf", SecondEdit))
        {
            Panic("A deleted /etc default did not come back or an edit was overwritten");
        }
    }
}

void CheckRootReadOnly(const char* kept, Ext2Data* data)
{
    if (VFS::Mount("ext4", "vda2", "/", Filesystem::MountMode::ReadOnly) !=
            VFS::MountResult::NotFound ||
        VFS::Mount("ext2", "vda1", "/", Filesystem::MountMode::ReadOnly) !=
            VFS::MountResult::NotFound ||
        VFS::Mount("ext4", "vda1", "/", Filesystem::MountMode::ReadOnly) != VFS::MountResult::Done)
    {
        Panic("Mounting the root read-only failed");
    }
    if (!MountedAs("/", "ext4", "vda1", true) || !SamplesMatch() ||
        (kept != nullptr && !HoldsExt2Data(kept, data)))
    {
        Panic("The ext4 root read back different contents");
    }
    VFS::FileHandle file{};
    if (!RefusesChanges("/samples") || VFS::Open("/samples/link", &file))
    {
        Panic("The read-only ext4 root took a change or opened a link");
    }
    if (VFS::Unmount("/") != VFS::MountResult::Done || VFS::IsDirectory("/samples/docs"))
    {
        Panic("Unmounting the read-only root failed");
    }
}

// Nothing runs yet, so once the other mounts are out of the way the root can be unmounted, mounted
// read-only and read-write again, and the FAT16 file written before must still be there. The
// root's changes of this boot are there too: unmounting the ext4 root closed its journal.
void CheckRemount(uint64_t boot_count, Ext2Data* data)
{
    constexpr const char* InnerPoints[] = {"/system", "/etc", "/dev", "/proc", "/tmp", FatPoint};
    constexpr char RootFile[] = "/BLOCK.TXT";
    if (VFS::Unmount("/") != VFS::MountResult::Busy)
    {
        Panic("Unmounting / with mounts inside it did not fail busy");
    }
    for (const char* point : InnerPoints)
    {
        if (VFS::IsMountPoint(point) && VFS::Unmount(point) != VFS::MountResult::Done)
        {
            Panic("Storage test could not unmount a filesystem inside /");
        }
    }
    VFS::FileHandle file;
    if (!VFS::Open(RootFile, &file) || VFS::Unmount("/") != VFS::MountResult::Busy)
    {
        Panic("Unmounting / with an open file did not fail busy");
    }
    VFS::Close(&file);
    if (VFS::Unmount("/") != VFS::MountResult::Done || VFS::Open(RootFile, &file))
    {
        Panic("Unmounting the root failed");
    }
    const char* kept = nullptr;
    if (boot_count == 1 || boot_count == 2)
    {
        kept = boot_count == 1 ? "/os/data.txt" : "/os/data-link.txt";
    }
    CheckRootReadOnly(kept, data);
    if (VFS::Mount("ext4", "vda1", "/") != VFS::MountResult::Done || VFS::IsReadOnly(RootFile))
    {
        Panic("Mounting the root read-write again failed");
    }
    if (!VFS::MountBuiltIn() || VFS::Mount("fat16", "vda2", FatPoint) != VFS::MountResult::Done ||
        !VFS::UseRootEtc())
    {
        Panic("Storage test could not mount the filesystems inside / again");
    }
    CheckPersistentFile(boot_count);
}

bool RefusesCreate(const char* point)
{
    char path[32];
    VFS::FileHandle file{};
    return String::FormatTo(path, sizeof(path), "{}/remount-new.txt", point) &&
           !VFS::Create(path, &file) && VFS::LastError() == User::SyscallErrorReadOnly;
}

// The partition's superblock, 1024 bytes in, names needs_recovery (incompatible feature 0x4).
bool PartitionNeedsRecovery(size_t number)
{
    FeatherDisk::Partition::Range ranges[16] = {};
    const size_t count = FeatherDisk::Partition::Find(BlockDevice::CoreDevice(), ranges);
    uint8_t sector[BlockDevice::SectorSize];
    if (number == 0 || number > count ||
        !BlockDevice::ReadSector(ranges[number - 1].first_sector + 2, sector))
    {
        Panic("Could not read the root's superblock");
    }
    constexpr uint32_t IncompatOffset = 0x60;
    constexpr uint8_t NeedsRecoveryBit = 0x4;
    return (sector[IncompatOffset] & NeedsRecoveryBit) != 0;
}

bool RemountTo(const char* point, Filesystem::MountMode mode, bool read_only, const char* type,
               const char* source)
{
    return VFS::Remount(point, mode) == VFS::MountResult::Done &&
           MountedAs(point, type, source, read_only);
}

// At the end of the storage test's mounts: the root and /fat change mode in place, the root's
// journal is closed on the way to read-only, and only the root's own /etc follows it.
void CheckRootRemount(uint64_t boot_count)
{
    constexpr char RemountFile[] = "/os/remount.txt";
    constexpr char Text[] = "remounted\n";
    VFS::FileHandle file{};
    VFS::Remove(RemountFile);
    if (!RemountTo("/", Filesystem::MountMode::ReadOnly, true, "ext4", "vda1") ||
        PartitionNeedsRecovery(1))
    {
        Panic("The root's read-only remount left its journal needing recovery");
    }
    if (!RefusesChanges("/samples") || !RefusesCreate("/etc") || !RefusesCreate("/") ||
        VFS::IsMountPoint("/etc"))
    {
        Panic("The read-only root or its /etc took a change");
    }
    const bool tmp_created = VFS::Create("/tmp/remount.txt", &file);
    VFS::Close(&file);
    if (!tmp_created || !VFS::Remove("/tmp/remount.txt"))
    {
        Panic("/tmp refused a change while the root was read-only");
    }
    if (!RemountTo("/", Filesystem::MountMode::ReadWrite, false, "ext4", "vda1") ||
        !WriteNew(RemountFile, Text, sizeof(Text) - 1))
    {
        Panic("The root did not remount read-write");
    }
    if (!RemountTo("/", Filesystem::MountMode::ReadOnly, true, "ext4", "vda1") ||
        PartitionNeedsRecovery(1) || !ReadsAs(RemountFile, Text) || !SamplesMatch() ||
        VFS::Create(RemountFile, &file))
    {
        Panic("The root read back wrong after a read-only remount");
    }
    if (!RemountTo("/", Filesystem::MountMode::ReadWrite, false, "ext4", "vda1") ||
        !VFS::Remove(RemountFile) || VFS::IsReadOnly("/etc/hosts"))
    {
        Panic("The root did not remount read-write again");
    }
    if (!RemountTo(FatPoint, Filesystem::MountMode::ReadOnly, true, "fat16", "vda2") ||
        !RefusesCreate(FatPoint) ||
        !RemountTo(FatPoint, Filesystem::MountMode::ReadWrite, false, "fat16", "vda2") ||
        !RemountTo(FatPoint, Filesystem::MountMode::ReadWrite, false, "fat16", "vda2"))
    {
        Panic("The FAT16 partition did not remount");
    }
    CheckPersistentFile(boot_count);
}

// The ext2 partition's files, which tools/create_test_disk.py makes.
bool Ext2FilesMatch()
{
    return ReadsAs("/ext2/hello.txt", "Hello from the ext2 partition.\n") &&
           ReadsAs("/ext2/docs/notes/deep.txt", "nested ext2 file\n") &&
           BigFileMatches("/ext2/big.txt", "ext2 line ", 5000) &&
           LinkTargetIs("/ext2/fast-link", "hello.txt");
}

void CheckExt2ReadOnly()
{
    VFS::FileHandle file{};
    if (VFS::Mount("ext2", "vda4", "/ext2", Filesystem::MountMode::ReadOnly) !=
            VFS::MountResult::Done ||
        !MountedAs("/ext2", "ext2", "vda4", true) || !Ext2FilesMatch() ||
        !RefusesChanges("/ext2") || VFS::Open("/ext2/fast-link", &file) ||
        VFS::Unmount("/ext2") != VFS::MountResult::Done || VFS::IsDirectory("/ext2/docs"))
    {
        Panic("A read-only ext2 mount took a change or opened a link");
    }
}

// The disk's third and fourth partitions, ext3 and ext2, mounted read-write; the first boot writes
// both and unmounts, the second finds the first boot's shell session's changes (ext2 synced by the
// writeback task, ext3 by replaying its journal). The originals are never changed.
void CheckExt3AndExt2(uint64_t boot_count, Ext2Data* data)
{
    if (VFS::Mount("ext2", "vda3", "/ext3") != VFS::MountResult::NotFound ||
        VFS::Mount("ext4", "vda3", "/ext3") != VFS::MountResult::NotFound ||
        VFS::Mount("ext3", "vda4", "/ext2") != VFS::MountResult::NotFound ||
        VFS::Mount("ext4", "vda4", "/ext2") != VFS::MountResult::NotFound ||
        VFS::Mount("ext2", "vda1", "/ext2") != VFS::MountResult::NotFound ||
        VFS::Mount("ext3", "vda1", "/ext2") != VFS::MountResult::NotFound)
    {
        Panic("Mounting an ext2, ext3 or ext4 partition by the wrong type did not fail");
    }
    if (boot_count == 2)
    {
        CheckReadOnlyBeforeReplay("ext3", "vda3", "/ext3");
    }
    CheckExt2ReadOnly();
    if (VFS::Mount("ext3", "vda3", "/ext3") != VFS::MountResult::Done ||
        VFS::Mount("ext2", "vda4", "/ext2") != VFS::MountResult::Done)
    {
        Panic("Mounting the ext3 and ext2 partitions failed");
    }
    if (!MountedAs("/ext3", "ext3", "vda3", false) || !MountedAs("/ext2", "ext2", "vda4", false))
    {
        Panic("An ext3 or ext2 partition did not mount writable");
    }
    if (!ReadsAs("/ext3/hello.txt", "Hello from the ext3 partition.\n") ||
        !ReadsAs("/ext3/docs/readme.txt", "ext3 subdirectory file\n") ||
        !BigFileMatches("/ext3/big.txt", "ext3 line ", 3000) ||
        !LinkTargetIs("/ext3/link", "docs/readme.txt") ||
        !ListsWith("/ext3/docs", 1, "readme.txt", User::ModeRegular))
    {
        Panic("The ext3 partition read back different contents");
    }
    if (!Ext2FilesMatch())
    {
        Panic("The ext2 partition read back different contents");
    }

    if (boot_count == 1)
    {
        WriteVolumeFirstBoot("/ext3", data);
        WriteVolumeFirstBoot("/ext2", data);
    }
    else if (boot_count == 2)
    {
        CheckReplayedFile("/ext3", "periodic ext3\n");
        if (!ReadsAs("/ext2/periodic.txt", "periodic ext2\n") || !VFS::Remove("/ext2/periodic.txt"))
        {
            Panic("The ext2 partition lost the file the first boot's writeback synced");
        }
        ChangeVolumeSecondBoot("/ext3", data);
        ChangeVolumeSecondBoot("/ext2", data);
    }
    if (VFS::Unmount("/ext3") != VFS::MountResult::Done ||
        VFS::Unmount("/ext2") != VFS::MountResult::Done || VFS::IsDirectory("/ext2/docs"))
    {
        Panic("Unmounting the ext3 and ext2 partitions failed");
    }
}

} // namespace

// Each boot reads the previous boot's count from the scratch sector, so running twice on one disk
// image shows that writes persist across a restart.
void RunStorageTests()
{
    uint8_t sector[BlockDevice::SectorSize];
    if (!BlockDevice::ReadSector(ScratchSector, sector))
    {
        Panic("Could not read the storage scratch sector");
    }
    uint64_t boot_count = 0;
    bool has_count = true;
    for (uint32_t index = 0; index < sizeof(BootCountMagic); ++index)
    {
        has_count = has_count && sector[index] == static_cast<uint8_t>(BootCountMagic[index]);
    }
    if (has_count)
    {
        for (uint32_t index = 0; index < sizeof(uint64_t); ++index)
        {
            boot_count |= static_cast<uint64_t>(sector[sizeof(BootCountMagic) + index])
                          << (8 * index);
        }
    }
    ++boot_count;

    constexpr uint32_t PatternStart = sizeof(BootCountMagic) + sizeof(uint64_t);
    for (uint32_t index = 0; index < sizeof(BootCountMagic); ++index)
    {
        sector[index] = static_cast<uint8_t>(BootCountMagic[index]);
    }
    for (uint32_t index = 0; index < sizeof(uint64_t); ++index)
    {
        sector[sizeof(BootCountMagic) + index] = static_cast<uint8_t>(boot_count >> (8 * index));
    }
    for (uint32_t index = PatternStart; index < BlockDevice::SectorSize; ++index)
    {
        sector[index] = PatternByte(index, boot_count);
    }
    if (!BlockDevice::WriteSector(ScratchSector, sector))
    {
        Panic("Could not write the storage scratch sector");
    }
    if (BlockDevice::WriteSector(BlockDevice::SectorCount(), sector))
    {
        Panic("A write past the end of the disk succeeded");
    }

    uint8_t read_back[BlockDevice::SectorSize];
    if (!BlockDevice::ReadSector(ScratchSector, read_back))
    {
        Panic("Could not read back the storage scratch sector");
    }
    for (uint32_t index = 0; index < BlockDevice::SectorSize; ++index)
    {
        if (read_back[index] != sector[index])
        {
            Panic("Storage scratch sector read back different data");
        }
    }

    MountFat16();
    RunFileTests(boot_count);
    const Memory::HeapObject<Ext2Data> data;
    if (!data)
    {
        Panic("No memory for the ext2, ext3 and ext4 write tests");
    }
    FillExt2Data(data->bytes);
    CheckRootFile();
    CheckRoot(boot_count, &*data);
    CheckEtc(boot_count);
    CheckExt3AndExt2(boot_count, &*data);
    CheckRemount(boot_count, &*data);
    CheckRootRemount(boot_count);
    if (VFS::Unmount(FatPoint) != VFS::MountResult::Done)
    {
        Panic("Unmounting the FAT16 partition failed");
    }

    char message[128];
    if (!String::FormatTo(message, sizeof(message),
                          "KERN/STORAGE: sector, FAT16, ext2, ext3 and ext4 file writes verified; "
                          "boot count {}\n",
                          boot_count))
    {
        Panic("Storage test message formatting failed");
    }
    Serial::Write(message);
}

void RunNetworkTests()
{
    if (!Network::IsUp())
    {
        Panic("Network test needs the virtio-net device");
    }
    // QEMU's user-mode gateway; this runs before init's DHCP client, so the request goes out from
    // 0.0.0.0.
    const Network::IpAddress gateway = Network::MakeIpAddress(Network::MakeIpv4(10, 0, 2, 2));
    Network::MacAddress gateway_mac;
    if (!Network::Resolve(gateway, &gateway_mac))
    {
        Panic("ARP got no answer from the gateway");
    }
    Network::MacAddress cached_mac;
    const uint64_t before = Time::TickCount();
    if (!Network::Resolve(gateway, &cached_mac) || !(cached_mac == gateway_mac) ||
        Time::TickCount() - before > 1)
    {
        Panic("ARP did not cache the gateway");
    }
    char mac[Network::FormattedMacLength];
    char address[Network::FormattedIpv4Length];
    Network::FormatMac(gateway_mac, mac);
    Network::FormatIpv4(Network::ToIpv4(gateway), address);
    char message[128];
    if (!String::FormatTo(message, sizeof(message), "KERN/NETWORK: ARP resolved gateway {} to {}\n",
                          static_cast<const char*>(address), static_cast<const char*>(mac)))
    {
        Panic("Network test message formatting failed");
    }
    Serial::Write(message);

    // QEMU's user-mode network advertises fec0::/64 and answers for fec0::2.
    Network::Ipv6Configuration ipv6;
    Network::ReadIpv6Configuration(&ipv6);
    if (Network::IsUnspecified(ipv6.global) || Network::IsUnspecified(ipv6.router))
    {
        Panic("IPv6 autoconfiguration got no router advertisement");
    }
    Network::IpAddress host = ipv6.global;
    for (size_t index = 8; index < sizeof(host.bytes); ++index)
    {
        host.bytes[index] = 0;
    }
    host.bytes[15] = 2;
    Network::MacAddress host_mac;
    if (!Network::Resolve(host, &host_mac))
    {
        Panic("Neighbor discovery got no answer from the host");
    }
    char ipv6_address[Network::FormattedIpv6Length];
    Network::FormatMac(host_mac, mac);
    Network::FormatIpv6(host, ipv6_address);
    if (!String::FormatTo(message, sizeof(message), "KERN/NETWORK: NDP resolved {} to {}\n",
                          static_cast<const char*>(ipv6_address), static_cast<const char*>(mac)))
    {
        Panic("Network test message formatting failed");
    }
    Serial::Write(message);
}

namespace
{

constexpr uint32_t ConsoleReportsPerSecond = 20;
constexpr size_t ConsoleReportLength = 192;
static_assert(Console::Count == 4);

void FormatConsoleReport(char* report)
{
    char names[Console::Count][33];
    for (uint32_t console = 0; console < Console::Count; ++console)
    {
        Process::ForegroundProgramName(Console::Terminal(console)->ForegroundGroup(),
                                       names[console], sizeof(names[console]));
    }
    if (!String::FormatTo(report, ConsoleReportLength,
                          "KERN/I: Consoles: tty{} shown; {}, {}, {}, {}", Console::Shown() + 1,
                          static_cast<const char*>(names[0]), static_cast<const char*>(names[1]),
                          static_cast<const char*>(names[2]), static_cast<const char*>(names[3])))
    {
        Panic("Console report formatting failed");
    }
}

uint64_t ConsoleReportTask(void*)
{
    char previous[ConsoleReportLength] = {};
    const uint64_t interval = Time::TicksPerSecond() / ConsoleReportsPerSecond;
    for (;;)
    {
        char report[ConsoleReportLength] = {};
        FormatConsoleReport(report);
        if (memcmp(report, previous, sizeof(report)) != 0)
        {
            Console::WriteSerialLine(report);
            memcpy(previous, report, sizeof(previous));
        }
        Scheduler::SleepUntil(Time::TickCount() + (interval == 0 ? 1 : interval));
    }
}

} // namespace

void StartConsoleReports()
{
    Scheduler::TaskId task;
    if (!Scheduler::CreateTask(ConsoleReportTask, nullptr, &task))
    {
        Panic("Could not start the console report task");
    }
}

namespace
{

void TriggerException()
{
#if defined(ARCH_x86_64)
    asm volatile("xor %%edx, %%edx\n\t"
                 "mov $1, %%eax\n\t"
                 "xor %%ecx, %%ecx\n\t"
                 "divl %%ecx"
                 :
                 :
                 : "rax", "rcx", "rdx", "cc");
#elif defined(ARCH_aarch64)
    asm volatile(".inst 0x00000000");
#endif
}

void RunInterruptTest(const void* platform_data)
{
    (void)platform_data;
#if defined(ARCH_x86_64)
    Arch::X86_64::RegisterInterruptHandler(0, OnPitInterrupt);
    Arch::X86_64::InitializeInterruptController();
    Arch::X86_64::StartPit(100);
    asm volatile("sti" ::: "memory");
    while (pit_interrupt_count == 0)
    {
        asm volatile("hlt" ::: "memory");
    }
    asm volatile("cli" ::: "memory");
    Serial::Write("KERN/IRQ: PIT interrupt acknowledged\n");
#elif defined(ARCH_aarch64)
    if (platform_data == nullptr)
    {
        Panic("AArch64 IRQ test requires a device tree");
    }
    const auto* dtb = static_cast<const uint8_t*>(platform_data);
    const uint32_t dtb_magic = (static_cast<uint32_t>(dtb[0]) << 24) |
                               (static_cast<uint32_t>(dtb[1]) << 16) |
                               (static_cast<uint32_t>(dtb[2]) << 8) | dtb[3];
    if (dtb_magic != 0xD00DFEED)
    {
        Panic("Limine provided an invalid device tree");
    }
    Arch::AArch64::RegisterInterruptHandler(0, OnGicInterrupt);
    if (!Arch::AArch64::InitializeInterruptController(dtb))
    {
        Panic("Unsupported AArch64 interrupt controller");
    }
    Arch::AArch64::TriggerSoftwareInterrupt(0);
    asm volatile("msr daifclr, #2" ::: "memory");
    while (gic_interrupt_count == 0)
    {
        asm volatile("wfi" ::: "memory");
    }
    asm volatile("msr daifset, #2" ::: "memory");
    Serial::Write("KERN/IRQ: GIC software interrupt acknowledged\n");
#endif
}

} // namespace

void Run(const void* platform_data)
{
    switch (SelectedMode())
    {
    case Mode::Scheduler:
        RunSchedulerTests(platform_data);
        break;
    case Mode::Timer:
        RunTimerTests(platform_data);
        break;
    case Mode::Memory:
        RunMemoryTests();
        break;
    case Mode::Panic:
        Panic("Intentional panic test");
    case Mode::Exception:
        TriggerException();
        break;
    case Mode::Interrupt:
        RunInterruptTest(platform_data);
        break;
    default:
        break;
    }
}

} // namespace FeatherOS::KernelTest