#include "calendar.h"
#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: date\n"
                        "Prints the current UTC date and time.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    const uint64_t seconds = ReadClock();
    char text[FormattedCalendarTimeLength];
    if (IsSyscallError(seconds) || !FormatCalendarTime(CalendarFromUnixSeconds(seconds), text))
    {
        WriteLine("date: clock unavailable");
        Exit(1);
    }
    WriteLine(text);
    Exit(0);
}
