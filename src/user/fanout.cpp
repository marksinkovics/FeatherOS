#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// fanout <count>: starts count `echo` children before waiting for any of them.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    constexpr uint64_t MaximumChildren = 16;
    uint64_t count;
    if (arguments == nullptr || arguments->argument_count != 2 ||
        !ParseUnsignedNumber(arguments->argument_values[1], &count) || count == 0 ||
        count > MaximumChildren)
    {
        WriteLine("usage: fanout <1-16>");
        Exit(1);
    }

    uint64_t children[MaximumChildren];
    for (uint64_t index = 0; index < count; ++index)
    {
        children[index] = Spawn("echo child");
        if (children[index] >= SyscallErrorBase)
        {
            WriteLine("fanout: spawn failed");
            Exit(1);
        }
    }
    for (uint64_t index = 0; index < count; ++index)
    {
        uint64_t status;
        if (Wait(children[index], &status) >= SyscallErrorBase || status != 0)
        {
            WriteLine("fanout: child failed");
            Exit(1);
        }
    }
    WriteText("fanout: ");
    WriteNumber(count);
    WriteLine(" children exited");
    Exit(0);
}
