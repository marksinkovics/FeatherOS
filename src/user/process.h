#ifndef FEATHEROS_USER_PROCESS_H
#define FEATHEROS_USER_PROCESS_H

#include "syscall_abi.h"

#include <stdint.h>

namespace FeatherOS::User
{

constexpr uint64_t MaximumCommandLineLength = 64;
constexpr uint64_t MaximumArgumentCount = 16;

// Read-only block the kernel maps into each program; _start receives a pointer to it.
struct ProgramArguments
{
    uint64_t process_id;
    uint64_t test_case;
    uint64_t user_preemption_test;
    // Null-terminated console line that launched the program, including the command name.
    char command_line[MaximumCommandLineLength];
    // The command line split at spaces; argument_values[0] is the command name.
    uint64_t argument_count;
    const char* argument_values[MaximumArgumentCount];
    char argument_storage[MaximumCommandLineLength];
    // The environment the parent passed to Spawn, in the same format.
    uint64_t environment_count;
    char environment[MaximumEnvironmentSize];
};

// The kernel maps the block as a single page.
static_assert(sizeof(ProgramArguments) <= 4096, "ProgramArguments must fit in one page");

} // namespace FeatherOS::User

#endif