#include "exceptions.h"

#include "local_apic.h"

#include "kernel/arch/serial.h"
#include "kernel/exception.h"
#include "kernel/process.h"
#include "kernel/scheduler.h"
#include "kernel/cpu.h"
#include "kernel/syscall.h"
#include "kernel/user_context.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Arch::X86_64
{
namespace
{

struct IdtEntry
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct IdtRegister
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct TaskStateSegment
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct ExceptionContext
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t instruction_pointer;
    uint64_t code_segment;
    uint64_t flags;
    uint64_t user_stack_pointer;
    uint64_t user_stack_segment;
};

// Each CPU needs its own TSS, so its own GDT for the TSS descriptor; the IDT is shared.
struct CpuTables
{
    alignas(16) uint64_t gdt[7];
    TaskStateSegment tss;
    alignas(16) uint8_t kernel_stack[16384];
    alignas(16) uint8_t double_fault_stack[16384];
    alignas(16) uint8_t nmi_stack[16384];
    alignas(16) uint8_t machine_check_stack[16384];
    alignas(16) uint8_t test_exception_stack[16384];
};

IdtEntry idt[256];
CpuTables cpu_tables[FeatherOS::Cpu::MaximumCpus];

TaskStateSegment& CurrentTss()
{
    return cpu_tables[FeatherOS::Cpu::CurrentIndex()].tss;
}

extern "C" void* x86_64_exception_stub_table[];
extern "C" void* x86_64_irq_stub_table[];
extern "C" void x86_64_spurious_interrupt();
extern "C" void x86_64_enable_sse();
extern "C" void x86_64_enter_user(uint64_t entry_point, uint64_t stack_pointer,
                                  uint64_t argument_pointer,
                                  FeatherOS::UserExecutionContext* user_context);
extern "C" void x86_64_user_test_return();
extern "C" void x86_64_syscall_stub();
extern "C" void x86_64_load_gdt(const IdtRegister* descriptor);
extern "C" void FeatherOS_x86_64_HandleHardwareInterrupt(uint64_t vector);
extern "C" void FeatherOS_x86_64_SetKernelStack(uint64_t stack_pointer);

const char* ExceptionName(uint64_t vector)
{
    static constexpr const char* names[] = {
        "Divide error",
        "Debug",
        "Non-maskable interrupt",
        "Breakpoint",
        "Overflow",
        "Bound range exceeded",
        "Invalid opcode",
        "Device not available",
        "Double fault",
        "Coprocessor segment overrun",
        "Invalid TSS",
        "Segment not present",
        "Stack-segment fault",
        "General protection fault",
        "Page fault",
        "Reserved",
        "x87 floating-point exception",
        "Alignment check",
        "Machine check",
        "SIMD floating-point exception",
        "Virtualization exception",
        "Control protection exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor injection exception",
        "VMM communication exception",
        "Security exception",
        "Reserved",
    };
    return vector < sizeof(names) / sizeof(names[0]) ? names[vector] : "Unknown exception";
}

void SetEntry(size_t vector, uintptr_t handler, uint16_t selector, uint8_t ist,
              uint8_t attributes = 0x8E)
{
    idt[vector] = {
        .offset_low = static_cast<uint16_t>(handler & 0xFFFF),
        .selector = selector,
        .ist = ist,
        .attributes = attributes,
        .offset_middle = static_cast<uint16_t>((handler >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>(handler >> 32),
        .reserved = 0,
    };
}

bool WriteSyscallCharacter(void*, char character)
{
    const char output[] = {character, '\0'};
    FeatherOS::Serial::Write(output);
    return true;
}

constexpr FeatherOS::Syscall::Handlers SerialSyscallHandlers = {WriteSyscallCharacter,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr};

template <size_t Size> uint64_t StackTop(uint8_t (&stack)[Size])
{
    return reinterpret_cast<uint64_t>(stack + Size);
}

void LoadCpuTables(CpuTables& cpu)
{
    TaskStateSegment& cpu_tss = cpu.tss;
    cpu_tss.rsp0 = StackTop(cpu.kernel_stack);
    cpu_tss.ist[0] = StackTop(cpu.double_fault_stack);
    cpu_tss.ist[1] = StackTop(cpu.nmi_stack);
    cpu_tss.ist[2] = StackTop(cpu.machine_check_stack);
    cpu_tss.ist[3] = StackTop(cpu.test_exception_stack);
    cpu_tss.iomap_base = sizeof(cpu_tss);

    uint64_t* gdt = cpu.gdt;
    gdt[1] = 0x00AF9A000000FFFF;
    gdt[2] = 0x00CF92000000FFFF;
    gdt[3] = 0x00CFF2000000FFFF;
    gdt[4] = 0x00AFFA000000FFFF;
    const uint64_t tss_base = reinterpret_cast<uint64_t>(&cpu_tss);
    const uint64_t tss_limit = sizeof(cpu_tss) - 1;
    gdt[5] = (tss_limit & 0xFFFF) | ((tss_base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((tss_limit >> 16) & 0xF) << 48) | (((tss_base >> 24) & 0xFF) << 56);
    gdt[6] = tss_base >> 32;
    const IdtRegister gdt_register = {
        .limit = sizeof(cpu.gdt) - 1,
        .base = reinterpret_cast<uint64_t>(gdt),
    };
    x86_64_load_gdt(&gdt_register);
}

void LoadIdt()
{
    const IdtRegister idt_register = {
        .limit = sizeof(idt) - 1,
        .base = reinterpret_cast<uint64_t>(idt),
    };
    asm volatile("lidt %0" : : "m"(idt_register));
}

} // namespace

void InitializeExceptions()
{
    asm volatile("cli" ::: "memory");
    x86_64_enable_sse();
    LoadCpuTables(cpu_tables[0]);

    for (size_t vector = 0; vector < 32; ++vector)
    {
        uint8_t ist = 0;
        if (vector == 8)
        {
            ist = 1;
        }
        else if (vector == 2)
        {
            ist = 2;
        }
        else if (vector == 18)
        {
            ist = 3;
        }
#if defined(FEATHEROS_EXCEPTION_TEST)
        else if (vector == 0)
        {
            ist = 4;
        }
#endif
        SetEntry(vector, reinterpret_cast<uintptr_t>(x86_64_exception_stub_table[vector]), 0x08,
                 ist);
    }
    // Vectors 32-47 are the legacy PIC; 48 and 49 are the local APIC IPI and timer.
    for (size_t irq = 0; irq < 18; ++irq)
    {
        SetEntry(32 + irq, reinterpret_cast<uintptr_t>(x86_64_irq_stub_table[irq]), 0x08, 0);
    }
    SetEntry(LocalApic::SpuriousVector, reinterpret_cast<uintptr_t>(x86_64_spurious_interrupt),
             0x08, 0);
    SetEntry(128, reinterpret_cast<uintptr_t>(x86_64_syscall_stub), 0x08, 0, 0xEE);
    LoadIdt();
}

void InitializeSecondaryCpu(uint32_t cpu_index)
{
    asm volatile("cli" ::: "memory");
    x86_64_enable_sse();
    LoadCpuTables(cpu_tables[cpu_index]);
    LoadIdt();
    // Drop translations cached before the kernel edited the shared page tables.
    asm volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");
}

extern "C" void FeatherOS_x86_64_SetKernelStack(uint64_t stack_pointer)
{
    CurrentTss().rsp0 = stack_pointer;
}

void RestoreCurrentUserKernelStack()
{
    auto* user_context =
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext());
    if (user_context != nullptr && user_context->active && user_context->kernel_stack_pointer != 0)
    {
        CurrentTss().rsp0 = user_context->kernel_stack_pointer;
    }
}

bool RunUserTest(uint64_t entry_point, uint64_t stack_pointer, uint64_t argument_pointer,
                 FeatherOS::UserExecutionContext* user_context)
{
    if (user_context == nullptr)
    {
        return false;
    }
    user_context->kernel_stack_pointer = 0;
    user_context->previous_kernel_stack_pointer = FeatherOS::Scheduler::CurrentKernelStackTop();
    if (user_context->previous_kernel_stack_pointer == 0)
    {
        return false;
    }
    user_context->exit_status = UINT64_MAX;
    user_context->active = true;
    user_context->exit_requested = false;
    FeatherOS::Scheduler::SetCurrentUserContext(user_context);
    x86_64_enter_user(entry_point, stack_pointer, argument_pointer, user_context);
    CurrentTss().rsp0 = user_context->previous_kernel_stack_pointer;
    user_context->active = false;
    FeatherOS::Scheduler::SetCurrentUserContext(nullptr);
    return user_context->exit_status == 0;
}

extern "C" uint64_t FeatherOS_x86_64_GetUserReturnStack()
{
    auto* user_context =
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext());
    if (user_context == nullptr || !user_context->active || !user_context->exit_requested)
    {
        return 0;
    }
    CurrentTss().rsp0 = user_context->previous_kernel_stack_pointer;
    user_context->active = false;
    return user_context->kernel_stack_pointer;
}

namespace
{

// A program that Ctrl+C interrupted ends when it would next resume in ring 3.
void TerminateIfKilled(const ExceptionContext* context)
{
    auto* user_context =
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext());
    if ((context->code_segment & 3) == 3 && user_context != nullptr && user_context->active &&
        user_context->kill_requested)
    {
        user_context->exit_status = FeatherOS::User::ExitStatusInterrupted;
        user_context->exit_requested = true;
    }
}

} // namespace

extern "C" void FeatherOS_x86_64_ExceptionDispatch(ExceptionContext* context)
{
    if (context->vector >= 32 && context->vector < 48)
    {
        FeatherOS_x86_64_HandleHardwareInterrupt(context->vector);
        RestoreCurrentUserKernelStack();
        TerminateIfKilled(context);
        return;
    }
    if (context->vector == LocalApic::IpiVector || context->vector == LocalApic::TimerVector ||
        context->vector == LocalApic::SpuriousVector)
    {
        LocalApic::HandleInterrupt(context->vector);
        RestoreCurrentUserKernelStack();
        TerminateIfKilled(context);
        return;
    }

    auto* user_context =
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext());
    if (context->vector == 128 && (context->code_segment & 3) == 3 && user_context != nullptr &&
        user_context->active)
    {
        const FeatherOS::Syscall::Handlers& handlers = user_context->syscall_handlers != nullptr
                                                           ? *user_context->syscall_handlers
                                                           : SerialSyscallHandlers;
        const FeatherOS::Syscall::Result result =
            FeatherOS::Syscall::Dispatch({context->rax, {context->rdi, context->rsi, context->rdx}},
                                         handlers, user_context->syscall_context);
        // A blocking syscall may have run other tasks, which repoint the TSS at their stacks.
        RestoreCurrentUserKernelStack();
        if (result.action == FeatherOS::Syscall::Action::Return)
        {
            context->rax = result.value;
            TerminateIfKilled(context);
            return;
        }
        user_context->exit_status = result.action == FeatherOS::Syscall::Action::Exit
                                        ? result.value
                                        : FeatherOS::User::ExitStatusFault;
        user_context->exit_requested = true;
        return;
    }
    if (context->vector < 32 && (context->code_segment & 3) == 3 && user_context != nullptr &&
        user_context->active)
    {
        // A CPU exception in user mode terminates the program instead of the kernel.
        bool kernel_mapping_isolated = false;
        if (context->vector == 14)
        {
            uint64_t fault_address;
            asm volatile("mov %%cr2, %0" : "=r"(fault_address));
            const bool not_present = (context->error_code & 1) == 0;
            if (not_present && FeatherOS::Process::HandleUserPageFault(user_context, fault_address))
            {
                return;
            }
            kernel_mapping_isolated = (context->error_code & 1) != 0 &&
                                      (context->error_code & 4) != 0 &&
                                      fault_address >= 0xFFFF800000000000;
        }
        user_context->exit_status = kernel_mapping_isolated
                                        ? FeatherOS::User::ExitStatusKernelAccessFault
                                        : FeatherOS::User::ExitStatusFault;
        user_context->exit_requested = true;
        return;
    }

    FeatherOS::ExceptionReport report;
    report.architecture = FeatherOS::ExceptionArchitecture::X86_64;
    report.name = ExceptionName(context->vector);
    report.vector = context->vector;
    report.instruction_pointer = context->instruction_pointer;
    report.error_code = context->error_code;
    report.syndrome = 0;
    report.fault_address = 0;
    if (context->vector == 14)
    {
        asm volatile("mov %%cr2, %0" : "=r"(report.fault_address));
    }
#if defined(FEATHEROS_MEMORY_TEST)
    if (FeatherOS::TryRecoverTestException(&report))
    {
        context->instruction_pointer = report.instruction_pointer;
        return;
    }
#endif
    FeatherOS::ReportFatalException(report);
}

} // namespace FeatherOS::Arch::X86_64