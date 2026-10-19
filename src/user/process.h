#ifndef FEATHEROS_USER_PROCESS_H
#define FEATHEROS_USER_PROCESS_H

#include <stdint.h>

namespace FeatherOS::User
{

constexpr uint64_t MaximumCommandLineLength = 64;
constexpr uint64_t MaximumArgumentCount = 8;

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
};

} // namespace FeatherOS::User

#endif