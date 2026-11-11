#include "command_line.h"

#include <chrono>
#include <print>
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
    const std::chrono::seconds up(since_boot.tv_sec);
    std::println("uptime: {}:{:%M:%S}", std::chrono::floor<std::chrono::hours>(up).count(), up);
    return 0;
}
