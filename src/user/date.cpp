#include "calendar.h"
#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// date: prints the current UTC wall-clock time with seconds.
extern "C" [[noreturn]] void _start(const ProgramArguments*)
{
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
