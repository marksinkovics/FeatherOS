#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

namespace
{

void WriteTwoDigits(uint64_t value)
{
    WriteCharacter('0' + value / 10);
    WriteCharacter('0' + value % 10);
}

} // namespace

// uptime: prints the time since boot as H:MM:SS.
extern "C" [[noreturn]] void _start(const ProgramArguments*)
{
    const uint64_t milliseconds = ReadUptime();
    if (IsSyscallError(milliseconds))
    {
        WriteLine("uptime: unavailable");
        Exit(1);
    }
    const uint64_t seconds = milliseconds / 1000;
    WriteText("uptime: ");
    WriteNumber(seconds / 3600);
    WriteCharacter(':');
    WriteTwoDigits(seconds / 60 % 60);
    WriteCharacter(':');
    WriteTwoDigits(seconds % 60);
    WriteCharacter('\n');
    Exit(0);
}
