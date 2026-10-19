#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// echo <words...>: prints its arguments separated by single spaces.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    if (arguments == nullptr)
    {
        Exit(2);
    }
    for (uint64_t index = 1; index < arguments->argument_count; ++index)
    {
        if (index > 1)
        {
            WriteCharacter(' ');
        }
        WriteText(arguments->argument_values[index]);
    }
    WriteCharacter('\n');
    Exit(0);
}
