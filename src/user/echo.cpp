#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: echo <words...>\n"
                        "Prints its arguments separated by single spaces.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
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
