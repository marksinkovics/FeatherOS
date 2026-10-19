#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// calc <number> <number>: prints the sum of two unsigned decimal numbers.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    uint64_t left;
    uint64_t right;
    if (arguments == nullptr || arguments->argument_count != 3 ||
        !ParseUnsignedNumber(arguments->argument_values[1], &left) ||
        !ParseUnsignedNumber(arguments->argument_values[2], &right))
    {
        WriteLine("usage: calc <number> <number>");
        Exit(1);
    }
    if (left > UINT64_MAX - right)
    {
        WriteLine("calc result overflow");
        Exit(1);
    }

    WriteText("calc: ");
    WriteNumber(left);
    WriteText(" + ");
    WriteNumber(right);
    WriteText(" = ");
    WriteNumber(left + right);
    WriteCharacter('\n');
    Exit(0);
}
