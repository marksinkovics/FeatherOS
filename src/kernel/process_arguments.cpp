#include "process_arguments.h"

#include <stddef.h>

namespace FeatherOS::Process
{

bool BuildProgramArguments(const char* command_line, uint64_t user_address,
                           User::ProgramArguments* arguments)
{
    if (command_line == nullptr || arguments == nullptr)
    {
        return false;
    }
    auto* bytes = reinterpret_cast<uint8_t*>(arguments);
    for (size_t index = 0; index < sizeof(*arguments); ++index)
    {
        bytes[index] = 0;
    }

    size_t length = 0;
    while (command_line[length] != '\0')
    {
        if (length == User::MaximumCommandLineLength - 1)
        {
            return false;
        }
        arguments->command_line[length] = command_line[length];
        ++length;
    }

    const uint64_t storage_address =
        user_address + offsetof(User::ProgramArguments, argument_storage);
    size_t storage_length = 0;
    size_t index = 0;
    while (index < length)
    {
        if (command_line[index] == ' ')
        {
            ++index;
            continue;
        }
        if (arguments->argument_count == User::MaximumArgumentCount)
        {
            return false;
        }
        arguments->argument_values[arguments->argument_count++] =
            reinterpret_cast<const char*>(storage_address + storage_length);
        // Double quotes group words, so "my notes.txt" is one argument; the quotes are dropped.
        bool quoted = false;
        while (index < length && (quoted || command_line[index] != ' '))
        {
            if (command_line[index] == '"')
            {
                quoted = !quoted;
                ++index;
                continue;
            }
            arguments->argument_storage[storage_length++] = command_line[index++];
        }
        if (quoted)
        {
            return false;
        }
        // Each argument is shorter than its source text plus separator, so the terminator fits.
        arguments->argument_storage[storage_length++] = '\0';
    }
    return true;
}

} // namespace FeatherOS::Process
