#include "command_line.h"

#include <stdio.h>
#include <time.h>

constexpr char Help[] = "usage: uptime\n"
                        "Prints the time since boot as H:MM:SS.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    timespec since_boot;
    if (clock_gettime(CLOCK_MONOTONIC, &since_boot) != 0)
    {
        puts("uptime: unavailable");
        return 1;
    }
    const long seconds = since_boot.tv_sec;
    printf("uptime: %ld:%02ld:%02ld\n", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return 0;
}
