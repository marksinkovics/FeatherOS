#include "command_line.h"

#include <stdio.h>
#include <time.h>

constexpr char Help[] =
    "usage: sleep <milliseconds>\n"
    "Waits at least that long without using a CPU, checked against the wall clock.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long milliseconds = 0;
    if (argc != 2 || !FeatherOS::User::ParseUnsigned(argv[1], &milliseconds))
    {
        puts("usage: sleep <milliseconds>");
        return 1;
    }
    const timespec duration = {static_cast<time_t>(milliseconds / 1000),
                               static_cast<long>(milliseconds % 1000 * 1000000)};
    const time_t start = time(nullptr);
    if (start == static_cast<time_t>(-1) || nanosleep(&duration, nullptr) != 0)
    {
        puts("sleep: failed");
        return 1;
    }
    const time_t end = time(nullptr);
    // The clock has one-second resolution, so only whole seconds can be checked.
    if (end == static_cast<time_t>(-1) ||
        static_cast<unsigned long long>(end - start) < milliseconds / 1000)
    {
        puts("sleep: woke too early");
        return 1;
    }
    printf("sleep: slept %llu ms\n", milliseconds);
    return 0;
}
