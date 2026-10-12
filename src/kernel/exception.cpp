#include "exception.h"

#include "kernel/arch/serial.h"
#include "kernel/panic.h"
#if defined(ARCH_x86_64)
#include "kernel/arch/x86_64/exceptions.h"
#elif defined(ARCH_aarch64)
#include "kernel/arch/aarch64/exceptions.h"
#endif

#include <stddef.h>

namespace FeatherOS
{
namespace
{

void WriteHex(uint64_t value)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    char text[] = "0x0000000000000000";
    for (size_t index = 0; index < 16; ++index)
    {
        const size_t shift = (15 - index) * 4;
        text[index + 2] = digits[(value >> shift) & 0xF];
    }
    Serial::Write(text);
}

} // namespace

void InitializeExceptions()
{
#if defined(ARCH_x86_64)
    Arch::X86_64::InitializeExceptions();
#elif defined(ARCH_aarch64)
    Arch::AArch64::InitializeExceptions();
#else
#error "Unsupported architecture for exception initialization"
#endif
}

[[noreturn]] void ReportFatalException(const ExceptionReport& report)
{
    if (report.architecture == ExceptionArchitecture::X86_64)
    {
        Serial::Write("KERN/EXCEPTION RIP: ");
        WriteHex(report.instruction_pointer);
        Serial::Write("\nKERN/EXCEPTION ERROR: ");
        WriteHex(report.error_code);
        Serial::Write("\n");
    }
    else
    {
        Serial::Write("KERN/EXCEPTION ESR: ");
        WriteHex(report.syndrome);
        Serial::Write("\nKERN/EXCEPTION FAR: ");
        WriteHex(report.fault_address);
        Serial::Write("\nKERN/EXCEPTION ELR: ");
        WriteHex(report.instruction_pointer);
        Serial::Write("\n");
    }

    Serial::Write("KERN/EXCEPTION: ");
    Serial::Write(report.name == nullptr ? "Unknown exception" : report.name);
    Serial::Write("\n");
    Panic("Unhandled CPU exception");
}

} // namespace FeatherOS