#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// sleep <milliseconds>: blocks for at least that long, checked against the wall clock.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    uint64_t milliseconds;
    if (arguments == nullptr || arguments->argument_count != 2 ||
        !ParseUnsignedNumber(arguments->argument_values[1], &milliseconds))
    {
        WriteLine("usage: sleep <milliseconds>");
        Exit(1);
    }
    const uint64_t start = ReadClock();
    if (IsSyscallError(start) || IsSyscallError(Sleep(milliseconds)))
    {
        WriteLine("sleep: failed");
        Exit(1);
    }
    const uint64_t end = ReadClock();
    // The clock has one-second resolution, so only whole seconds can be checked.
    if (IsSyscallError(end) || end - start < milliseconds / 1000)
    {
        WriteLine("sleep: woke too early");
        Exit(1);
    }
    WriteText("sleep: slept ");
    WriteNumber(milliseconds);
    WriteLine(" ms");
    Exit(0);
}
