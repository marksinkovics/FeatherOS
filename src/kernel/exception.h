#ifndef FEATHEROS_EXCEPTION_H
#define FEATHEROS_EXCEPTION_H

#include <stdint.h>

namespace FeatherOS
{

enum class ExceptionArchitecture : uint8_t
{
    X86_64,
    AArch64,
};

struct ExceptionReport
{
    ExceptionArchitecture architecture;
    const char* name;
    uint64_t vector;
    uint64_t instruction_pointer;
    uint64_t error_code;
    uint64_t syndrome;
    uint64_t fault_address;
};

void InitializeExceptions();
[[noreturn]] void ReportFatalException(const ExceptionReport& report);

} // namespace FeatherOS

#endif