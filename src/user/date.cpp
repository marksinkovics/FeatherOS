#include "command_line.h"

#include <stdio.h>
#include <time.h>

constexpr char Help[] = "usage: date\n"
                        "Prints the current UTC date and time.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    const time_t now = time(nullptr);
    char text[32];
    if (now == static_cast<time_t>(-1) ||
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", gmtime(&now)) == 0)
    {
        puts("date: clock unavailable");
        return 1;
    }
    puts(text);
    return 0;
}
