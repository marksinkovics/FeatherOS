#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: upper\n"
                        "Reads one line at an upper> prompt and prints it in upper case.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
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
