#ifndef FEATHEROS_AARCH64_EXCEPTIONS_H
#define FEATHEROS_AARCH64_EXCEPTIONS_H

#include "kernel/user_context.h"

#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{

void InitializeExceptions();
bool RunUserTest(uint64_t entry_point, uint64_t stack_pointer, uint64_t argument_pointer,
                 UserExecutionContext* user_context);

} // namespace FeatherOS::Arch::AArch64

#endif