#include "exceptions.h"

#include "kernel/arch/serial.h"
#include "kernel/exception.h"
#include "kernel/process.h"
#include "kernel/panic.h"
#include "kernel/scheduler.h"
#include "kernel/syscall.h"
#include "kernel/user_context.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{
namespace
{

struct ExceptionContext
{
    uint64_t registers[31];
    uint64_t esr;
    uint64_t far;
    uint64_t elr;
    uint64_t spsr;
};

// The frame aarch64_irq_common saves.
struct InterruptContext
{
    uint64_t registers[31];
    uint64_t elr;
    uint64_t spsr;
};

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
                                                                nullptr};

} // namespace

extern "C" uint8_t aarch64_exception_vectors[];
extern "C" void aarch64_enter_user(uint64_t entry_point, uint64_t stack_pointer,
                                   uint64_t argument_pointer,
                                   FeatherOS::UserExecutionContext* user_context);
extern "C" void aarch64_user_test_return();

void InitializeExceptions()
{
    uint64_t current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    if ((current_el & 0xC) != 0x4)
    {
        FeatherOS::Panic("AArch64 exception setup requires EL1");
    }

    // Limine enters on SP_EL0 (and leaves SP_EL1 zero on secondary CPUs). Exceptions always push
    // onto SP_EL1, so the kernel moves its stack there; then every frame lands on the current
    // task's kernel stack, and SP_EL0 only ever holds user stack pointers.
    uint64_t stack_select;
    asm volatile("mrs %0, SPSel" : "=r"(stack_select));
    if (stack_select == 0)
    {
        asm volatile("mov x9, sp\n\tmsr SPSel, #1\n\tmov sp, x9" ::: "x9", "memory");
    }

    const uint64_t vector_address = reinterpret_cast<uint64_t>(aarch64_exception_vectors);
    asm volatile("msr VBAR_EL1, %0\n\tisb" : : "r"(vector_address) : "memory");
}

void InitializeSecondaryCpu(uint32_t)
{
    InitializeExceptions();
    // Drop translations cached before the kernel edited the shared page tables.
    asm volatile("dsb ishst\n\ttlbi vmalle1\n\tdsb nsh\n\tisb" ::: "memory");
}

bool RunUserTest(uint64_t entry_point, uint64_t stack_pointer, uint64_t argument_pointer,
                 FeatherOS::UserExecutionContext* user_context)
{
    if (user_context == nullptr)
    {
        return false;
    }
    user_context->kernel_stack_pointer = 0;
    user_context->exit_status = UINT64_MAX;
    user_context->active = true;
    user_context->exit_requested = false;
    FeatherOS::Scheduler::SetCurrentUserContext(user_context);
    aarch64_enter_user(entry_point, stack_pointer, argument_pointer, user_context);
    user_context->active = false;
    FeatherOS::Scheduler::SetCurrentUserContext(nullptr);
    return user_context->exit_status == 0;
}

namespace
{

// Makes the exception return resume aarch64_user_test_return in EL1, which ends the program.
void ReturnToKernel(FeatherOS::UserExecutionContext* user_context, uint64_t* x0, uint64_t* elr,
                    uint64_t* spsr)
{
    *x0 = reinterpret_cast<uint64_t>(user_context);
    *elr = reinterpret_cast<uint64_t>(aarch64_user_test_return);
    *spsr = (*spsr & ~0xFULL) | 0x5;
}

// A program that Ctrl+C interrupted ends when it would next resume in EL0.
void TerminateIfKilled(FeatherOS::UserExecutionContext* user_context, uint64_t* x0, uint64_t* elr,
                       uint64_t* spsr)
{
    if ((*spsr & 0xF) == 0 && user_context != nullptr && user_context->active &&
        user_context->kill_requested)
    {
        user_context->exit_status = FeatherOS::User::ExitStatusInterrupted;
        user_context->exit_requested = true;
        ReturnToKernel(user_context, x0, elr, spsr);
    }
}

} // namespace

extern "C" void FeatherOS_AArch64_InterruptReturn(InterruptContext* context)
{
    TerminateIfKilled(
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext()),
        &context->registers[0], &context->elr, &context->spsr);
}

extern "C" void FeatherOS_AArch64_ExceptionDispatch(ExceptionContext* context)
{
    const uint64_t exception_class = (context->esr >> 26) & 0x3F;
    const bool from_el0 = (context->spsr & 0xF) == 0;
    auto* user_context =
        static_cast<FeatherOS::UserExecutionContext*>(FeatherOS::Scheduler::CurrentUserContext());
    if (user_context != nullptr && user_context->active && from_el0 && exception_class == 0x15)
    {
        const FeatherOS::Syscall::Handlers& handlers = user_context->syscall_handlers != nullptr
                                                           ? *user_context->syscall_handlers
                                                           : SerialSyscallHandlers;
        const FeatherOS::Syscall::Result result = FeatherOS::Syscall::Dispatch(
            {context->registers[8],
             {context->registers[0], context->registers[1], context->registers[2]}},
            handlers, user_context->syscall_context);
        if (result.action == FeatherOS::Syscall::Action::Return)
        {
            context->registers[0] = result.value;
            TerminateIfKilled(user_context, &context->registers[0], &context->elr, &context->spsr);
            return;
        }
        user_context->exit_status = result.action == FeatherOS::Syscall::Action::Exit
                                        ? result.value
                                        : FeatherOS::User::ExitStatusFault;
        user_context->exit_requested = true;
        ReturnToKernel(user_context, &context->registers[0], &context->elr, &context->spsr);
        return;
    }
    if (user_context != nullptr && user_context->active && from_el0)
    {
        // A synchronous exception in EL0 terminates the program instead of the kernel.
        const uint64_t fault_status = context->esr & 0x3F;
        const bool translation_fault = fault_status >= 0x04 && fault_status <= 0x07;
        if (exception_class == 0x24 && translation_fault &&
            FeatherOS::Process::HandleUserPageFault(user_context, context->far))
        {
            return;
        }
        const bool permission_fault = fault_status >= 0x0D && fault_status <= 0x0F;
        const bool kernel_mapping_isolated =
            exception_class == 0x24 && permission_fault && context->far >= 0xFFFF800000000000;
        user_context->exit_status = kernel_mapping_isolated
                                        ? FeatherOS::User::ExitStatusKernelAccessFault
                                        : FeatherOS::User::ExitStatusFault;
        user_context->exit_requested = true;
        ReturnToKernel(user_context, &context->registers[0], &context->elr, &context->spsr);
        return;
    }
    const char* name =
        exception_class == 0 ? "AArch64 undefined instruction" : "AArch64 synchronous exception";

    FeatherOS::ExceptionReport report;
    report.architecture = FeatherOS::ExceptionArchitecture::AArch64;
    report.name = name;
    report.vector = exception_class;
    report.instruction_pointer = context->elr;
    report.error_code = 0;
    report.syndrome = context->esr;
    report.fault_address = context->far;
#if defined(FEATHEROS_MEMORY_TEST)
    if (FeatherOS::TryRecoverTestException(&report))
    {
        context->elr = report.instruction_pointer;
        return;
    }
#endif
    FeatherOS::ReportFatalException(report);
}

} // namespace FeatherOS::Arch::AArch64