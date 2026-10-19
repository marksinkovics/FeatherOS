#ifndef FEATHEROS_PROCESS_ARGUMENTS_H
#define FEATHEROS_PROCESS_ARGUMENTS_H

#include "user/process.h"

#include <stdint.h>

namespace FeatherOS::Process
{

// Fills arguments from command_line for a block mapped at user_address: copies the line, splits
// it at spaces, and points argument_values at user addresses inside argument_storage. Other
// fields are zeroed. Fails if the line is too long or has too many arguments.
bool BuildProgramArguments(const char* command_line, uint64_t user_address,
                           User::ProgramArguments* arguments);

} // namespace FeatherOS::Process

#endif
