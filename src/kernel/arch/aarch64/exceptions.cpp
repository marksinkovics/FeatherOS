#include "exceptions.h"

#include "kernel/exception.h"
#include "kernel/panic.h"

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

} // namespace

extern "C" uint8_t aarch64_exception_vectors[];

void InitializeExceptions()
{
    uint64_t current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    if ((current_el & 0xC) != 0x4)
    {
        FeatherOS::Panic("AArch64 exception setup requires EL1");
    }

    const uint64_t vector_address = reinterpret_cast<uint64_t>(aarch64_exception_vectors);
    asm volatile("msr VBAR_EL1, %0\n\tisb" : : "r"(vector_address) : "memory");
}

extern "C" [[noreturn]] void FeatherOS_AArch64_ExceptionDispatch(const ExceptionContext* context)
{
    const uint64_t exception_class = (context->esr >> 26) & 0x3F;
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
    FeatherOS::ReportFatalException(report);
}

} // namespace FeatherOS::Arch::AArch64