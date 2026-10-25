#include "exceptions.h"

#include "local_apic.h"

#include "kernel/arch/floating_point.h"
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

constexpr uint64_t UserAddressLimit = 0x0000800000000000;
// The flags a program may change: CF, PF, AF, ZF, SF, DF and OF. IF and IOPL stay as the kernel
// set them.
constexpr uint64_t UserFlags = 0x0CD5;
constexpr uint64_t TrapFlag = 1U << 8;
constexpr uint64_t DirectionFlag = 1U << 10;
// The ABI lets a function use 128 bytes below the stack pointer without moving it.
constexpr uint64_t RedZone = 128;
constexpr uint32_t MxcsrOffset = 24;
// The MXCSR bits FXRSTOR accepts on every x86-64 CPU; a reserved bit would fault in the kernel.
constexpr uint32_t MxcsrMask = 0xFFBF;

// What a signal handler's frame keeps on the user stack, below the interrupted stack pointer.
struct SignalFrame
{
    // The handler returns here: the C library's restorer, which makes the SignalReturn syscall.
    uint64_t return_address;
    uint64_t signal;
    uint64_t saved_mask;
    uint64_t registers[15];
    uint64_t instruction_pointer;
    uint64_t flags;
    uint64_t stack_pointer;
    FeatherOS::Arch::FloatingPoint::State floating_point;
};

constexpr uint64_t ExceptionContext::* SavedRegisters[] = {
    &ExceptionContext::r15, &ExceptionContext::r14, &ExceptionContext::r13, &ExceptionContext::r12,
    &ExceptionContext::r11, &ExceptionContext::r10, &ExceptionContext::r9,  &ExceptionContext::r8,
    &ExceptionContext::rbp, &ExceptionContext::rdi, &ExceptionContext::rsi, &ExceptionContext::rdx,
    &ExceptionContext::rcx, &ExceptionContext::rbx, &ExceptionContext::rax,
};

void TerminateForSignal(FeatherOS::UserExecutionContext* user_context, int signal)
{
    user_context->exit_status = FeatherOS::User::ExitStatusSignalBase + signal;
    user_context->exit_requested = true;
}

// Builds the handler's frame and points the return to ring 3 at the handler, which is called
// with the signal number as if from the interrupted instruction.
bool EnterHandler(ExceptionContext* context, FeatherOS::UserExecutionContext* user_context,
                  const FeatherOS::Signals::Decision& decision)
{
    SignalFrame frame;
    frame.return_address = decision.action.restorer;
    frame.signal = static_cast<uint64_t>(decision.signal);
    frame.saved_mask = decision.saved_mask;
    for (size_t index = 0; index < sizeof(SavedRegisters) / sizeof(SavedRegisters[0]); ++index)
    {
        frame.registers[index] = context->*SavedRegisters[index];
    }
    frame.instruction_pointer = context->instruction_pointer;
    frame.flags = context->flags;
    frame.stack_pointer = context->user_stack_pointer;
    FeatherOS::Arch::FloatingPoint::Save(&frame.floating_point);
    if (context->user_stack_pointer < RedZone + sizeof(frame) + 16)
    {
        return false;
    }
    // At the handler's first instruction the stack is as just after a call: 8 bytes past a
    // 16-byte boundary.
    const uint64_t address =
        ((context->user_stack_pointer - RedZone - sizeof(frame)) & ~uint64_t{15}) - 8;
    if (!FeatherOS::Process::CopyToCurrentUser(user_context, address, &frame, sizeof(frame)))
    {
        return false;
    }
    context->instruction_pointer = decision.action.handler;
    context->user_stack_pointer = address;
    context->rdi = static_cast<uint64_t>(decision.signal);
    context->rsi = 0;
    context->rdx = 0;
    context->flags &= ~(DirectionFlag | TrapFlag);
    return true;
}

// Every return to ring 3 passes here: the program takes its next signal, which may end it.
void DeliverSignals(ExceptionContext* context)
{
    auto* user_context =
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext());
    if ((context->code_segment & 3) != 3 || user_context == nullptr || !user_context->active ||
        user_context->exit_requested)
    {
        return;
    }
    const FeatherOS::Signals::Decision decision =
        FeatherOS::Signals::TakeNext(&user_context->signals);
    if (decision.kind == FeatherOS::Signals::DecisionKind::Terminate)
    {
        TerminateForSignal(user_context, decision.signal);
    }
    // A program whose stack cannot hold the frame cannot run the handler.
    else if (decision.kind == FeatherOS::Signals::DecisionKind::Handle &&
             !EnterHandler(context, user_context, decision))
    {
        TerminateForSignal(user_context, FeatherOS::User::SignalSegmentationFault);
    }
}

// Puts back what the frame saved, refusing what a program could use to escape ring 3: kernel
// addresses, privileged flags, and MXCSR bits that would fault in FXRSTOR.
bool ReturnFromSignal(ExceptionContext* context, FeatherOS::UserExecutionContext* user_context)
{
    SignalFrame frame;
    // The handler's ret popped the return address.
    const uint64_t address = context->user_stack_pointer - 8;
    if (!FeatherOS::Process::CopyFromCurrentUser(user_context, address, &frame, sizeof(frame)) ||
        frame.instruction_pointer >= UserAddressLimit || frame.stack_pointer >= UserAddressLimit)
    {
        return false;
    }
    for (size_t index = 0; index < sizeof(SavedRegisters) / sizeof(SavedRegisters[0]); ++index)
    {
        context->*SavedRegisters[index] = frame.registers[index];
    }
    context->instruction_pointer = frame.instruction_pointer;
    context->user_stack_pointer = frame.stack_pointer;
    context->flags = (context->flags & ~UserFlags) | (frame.flags & UserFlags);
    uint8_t* mxcsr = frame.floating_point.bytes + MxcsrOffset;
    const uint32_t value =
        static_cast<uint32_t>(mxcsr[0]) | (static_cast<uint32_t>(mxcsr[1]) << 8) |
        (static_cast<uint32_t>(mxcsr[2]) << 16) | (static_cast<uint32_t>(mxcsr[3]) << 24);
    const uint32_t sanitized = value & MxcsrMask;
    for (uint32_t byte = 0; byte < 4; ++byte)
    {
        mxcsr[byte] = static_cast<uint8_t>(sanitized >> (8 * byte));
    }
    FeatherOS::Arch::FloatingPoint::Restore(&frame.floating_point);
    FeatherOS::Signals::RestoreMask(&user_context->signals, frame.saved_mask);
    return true;
}

} // namespace

extern "C" void FeatherOS_x86_64_ExceptionDispatch(ExceptionContext* context)
{
    if (context->vector >= 32 && context->vector < 48)
    {
        FeatherOS_x86_64_HandleHardwareInterrupt(context->vector);
        RestoreCurrentUserKernelStack();
        DeliverSignals(context);
        return;
    }
    if (context->vector == LocalApic::IpiVector || context->vector == LocalApic::TimerVector ||
        context->vector == LocalApic::SpuriousVector)
    {
        LocalApic::HandleInterrupt(context->vector);
        RestoreCurrentUserKernelStack();
        DeliverSignals(context);
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
            DeliverSignals(context);
            return;
        }
        if (result.action == FeatherOS::Syscall::Action::SignalReturn)
        {
            if (!ReturnFromSignal(context, user_context))
            {
                TerminateForSignal(user_context, FeatherOS::User::SignalSegmentationFault);
                return;
            }
            DeliverSignals(context);
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