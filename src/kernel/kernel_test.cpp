#include "kernel_test.h"

#include "kernel/arch/serial.h"
#include "kernel/exception.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/kernel_stack.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/panic.h"
#if defined(FEATHEROS_SCHEDULER_TEST) || defined(FEATHEROS_SMP_TEST)
#include "kernel/scheduler.h"
#endif
#if defined(FEATHEROS_SMP_TEST)
#include "kernel/cpu.h"
#endif
#include "kernel/string/format.h"
#if defined(FEATHEROS_TIMER_TEST) || defined(FEATHEROS_SCHEDULER_TEST) ||                          \
    defined(FEATHEROS_SMP_TEST)
#include "kernel/time/timer.h"
#endif

#if defined(ARCH_x86_64) && defined(FEATHEROS_INTERRUPT_TEST)
#include "kernel/arch/x86_64/interrupts.h"
#elif defined(ARCH_aarch64) && defined(FEATHEROS_INTERRUPT_TEST)
#include "kernel/arch/aarch64/interrupts.h"
#endif

#include <stdint.h>

#if defined(FEATHEROS_MEMORY_TEST)
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
#endif

#if defined(ARCH_aarch64) && (defined(FEATHEROS_INTERRUPT_TEST) || defined(FEATHEROS_TIMER_TEST))
namespace
{

#if defined(FEATHEROS_INTERRUPT_TEST)
volatile uint64_t gic_interrupt_count = 0;

void OnGicInterrupt()
{
    ++gic_interrupt_count;
}
#endif

} // namespace
#elif defined(ARCH_x86_64) && defined(FEATHEROS_INTERRUPT_TEST)
namespace
{

volatile uint64_t pit_interrupt_count = 0;

void OnPitInterrupt()
{
    ++pit_interrupt_count;
}

} // namespace
#endif

#if defined(FEATHEROS_TIMER_TEST)
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
#endif

namespace FeatherOS::KernelTest
{

#if defined(FEATHEROS_SCHEDULER_TEST)
namespace
{

uint32_t scheduler_sequence = 0;
volatile uint64_t preemption_progress[2] = {};
Sync::SpinLock blocking_test_lock;
Scheduler::WaitQueue blocking_test_queue;
Scheduler::WaitQueue timeout_test_queue;
bool blocking_test_signaled = false;

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
        ++preemption_progress[0];
    }
    return 53;
}

uint64_t SecondPreemptedTask(void*)
{
    do
    {
        ++preemption_progress[1];
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
    blocking_test_signaled = false;
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
    Serial::Write("KERN/SCHEDULER: preemption, wait, exit, reap, blocking, and sleep passed\n");
}

} // namespace
#endif

#if defined(FEATHEROS_MEMORY_TEST)
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
    auto* user_value = reinterpret_cast<volatile uint64_t*>(user_test_address);
    *user_value = 0xA11CE55A1234FEDC;
    const volatile uint64_t observed_user_value = *user_value;
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
#endif

#if defined(FEATHEROS_SMP_TEST)
namespace
{

constexpr uint32_t SmpTestTasks = 3;
constexpr uint64_t SmpCounterIterations = 20000;
constexpr uint64_t SmpStartTimeoutTicks = 200;

Sync::SpinLock smp_counter_lock;
uint64_t smp_counter = 0;
uint32_t smp_started_tasks = 0;
uint32_t smp_concurrent_tasks = 0;
uint32_t smp_cpus_seen = 0;

uint64_t SmpTask(void*)
{
    __atomic_fetch_or(&smp_cpus_seen, 1U << Cpu::CurrentIndex(), __ATOMIC_RELAXED);
    __atomic_fetch_add(&smp_started_tasks, 1U, __ATOMIC_ACQ_REL);
    // Preemption is off, so every task is running at once only if each has its own CPU.
    const uint64_t deadline = Time::TickCount() + SmpStartTimeoutTicks;
    while (__atomic_load_n(&smp_started_tasks, __ATOMIC_ACQUIRE) < SmpTestTasks &&
           Time::TickCount() < deadline)
    {
        Sync::CpuRelax();
    }
    if (__atomic_load_n(&smp_started_tasks, __ATOMIC_ACQUIRE) == SmpTestTasks)
    {
        __atomic_fetch_add(&smp_concurrent_tasks, 1U, __ATOMIC_RELAXED);
    }
    for (uint64_t iteration = 0; iteration < SmpCounterIterations; ++iteration)
    {
        Sync::SpinLockGuard guard(smp_counter_lock);
        ++smp_counter;
    }
    return 0;
}

} // namespace

void RunSmpTests()
{
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
    for (uint32_t cpus = smp_cpus_seen; cpus != 0; cpus &= cpus - 1)
    {
        ++cpus_used;
    }
    if (smp_concurrent_tasks != SmpTestTasks || cpus_used != SmpTestTasks ||
        smp_counter != SmpTestTasks * SmpCounterIterations)
    {
        Panic("Tasks did not run concurrently on separate CPUs");
    }
    Serial::Write("KERN/SMP: tasks ran concurrently on 3 CPUs; locked counter exact\n");
}
#endif

void Run(const void* platform_data)
{
    (void)platform_data;
#if defined(FEATHEROS_SCHEDULER_TEST)
    RunSchedulerTests(platform_data);
#endif
#if defined(FEATHEROS_TIMER_TEST)
    RunTimerTests(platform_data);
#endif

#if defined(FEATHEROS_MEMORY_TEST)
    RunMemoryTests();
#endif

#if defined(FEATHEROS_PANIC_TEST)
    Panic("Intentional panic test");
#endif

#if defined(FEATHEROS_EXCEPTION_TEST) && defined(ARCH_x86_64)
    asm volatile("xor %%edx, %%edx\n\t"
                 "mov $1, %%eax\n\t"
                 "xor %%ecx, %%ecx\n\t"
                 "divl %%ecx"
                 :
                 :
                 : "rax", "rcx", "rdx", "cc");
#elif defined(FEATHEROS_EXCEPTION_TEST) && defined(ARCH_aarch64)
    asm volatile(".inst 0x00000000");
#endif

#if defined(FEATHEROS_INTERRUPT_TEST) && defined(ARCH_x86_64)
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
#elif defined(FEATHEROS_INTERRUPT_TEST) && defined(ARCH_aarch64)
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

} // namespace FeatherOS::KernelTest