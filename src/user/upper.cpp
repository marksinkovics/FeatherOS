#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// upper: reads one line of console input and prints it in upper case.
extern "C" [[noreturn]] void _start(const ProgramArguments*)
{
    char line[MaximumCommandLineLength];
    const uint64_t length = ReadLine(line, sizeof(line), "upper> ");
    if (IsSyscallError(length))
    {
        WriteLine("upper: could not read input");
        Exit(1);
    }
    for (uint64_t index = 0; index < length; ++index)
    {
        const char character = line[index];
        WriteCharacter(static_cast<uint64_t>(
            character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character));
    }
    WriteCharacter('\n');
    Exit(0);
}
