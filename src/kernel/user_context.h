#ifndef FEATHEROS_USER_CONTEXT_H
#define FEATHEROS_USER_CONTEXT_H

#include "kernel/syscall.h"

#include <stdint.h>

namespace FeatherOS
{

// The user-entry assembly reads fields by offset; append new fields at the end.
struct UserExecutionContext
{
    uint64_t address_space_root_physical_address;
    uint64_t kernel_stack_pointer;
    uint64_t previous_kernel_stack_pointer;
    uint64_t exit_status;
    bool active;
    bool exit_requested;
    bool preemption_enabled;
    // Syscall services for this program; when null, only exit and serial output are available.
    const Syscall::Handlers* syscall_handlers;
    void* syscall_context;
    // The Process::UserProgram that owns this context.
    void* program;
};

} // namespace FeatherOS

#endif
