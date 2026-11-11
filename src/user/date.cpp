#include "command_line.h"

#include <chrono>
#include <print>
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
    if (now == static_cast<time_t>(-1))
    {
        puts("date: clock unavailable");
        return 1;
    }
    std::println("{:%F %T %Z}", std::chrono::sys_seconds(std::chrono::seconds(now)));
    return 0;
}
