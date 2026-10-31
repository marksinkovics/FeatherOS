#include "command_line.h"

#include <stdio.h>
#include <time.h>

constexpr char Help[] =
    "usage: sleep <seconds>\n"
    "Waits at least that long, such as 2 or 0.5, without using a CPU, checked against the\n"
    "wall clock.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long nanoseconds = 0;
    if (argc != 2 || !FeatherOS::User::ParseSeconds(argv[1], &nanoseconds))
    {
        puts("usage: sleep <seconds>");
        return 1;
    }
    const unsigned long long seconds = nanoseconds / 1000000000;
    const timespec duration = {static_cast<time_t>(seconds),
                               static_cast<long>(nanoseconds % 1000000000)};
    const time_t start = time(nullptr);
    if (start == static_cast<time_t>(-1) || nanosleep(&duration, nullptr) != 0)
    {
        puts("sleep: failed");
        return 1;
    }
    const time_t end = time(nullptr);
    // The clock has one-second resolution, so only whole seconds can be checked.
    if (end == static_cast<time_t>(-1) || static_cast<unsigned long long>(end - start) < seconds)
    {
        puts("sleep: woke too early");
        return 1;
    }
    printf("sleep: slept %s s\n", argv[1]);
    return 0;
}
