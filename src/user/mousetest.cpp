#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <featheros/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

constexpr char Help[] =
    "usage: mousetest [count]\n"
    "Reads count events (default 16) from /dev/mouse and prints each one: \"mouse: rel x 10\",\n"
    "\"mouse: rel y -3\", \"mouse: rel wheel 1\", \"mouse: rel hwheel 1\", \"mouse: abs x 16383\"\n"
    "(a tablet's position, 0 to 32767), \"mouse: abs y 0\", \"mouse: key 272 1\" (a button code "
    "and\n"
    "1 for down, 0 for up), \"mouse: syn\" at the end of each report and \"mouse: dropped\" if "
    "events\n"
    "were lost.";

constexpr unsigned long long DefaultCount = 16;

namespace
{

const char* RelativeName(unsigned code)
{
    switch (code)
    {
    case REL_X:
        return "x";
    case REL_Y:
        return "y";
    case REL_WHEEL:
        return "wheel";
    case REL_HWHEEL:
        return "hwheel";
    default:
        return nullptr;
    }
}

const char* AbsoluteName(unsigned code)
{
    switch (code)
    {
    case ABS_X:
        return "x";
    case ABS_Y:
        return "y";
    default:
        return nullptr;
    }
}

void Print(const input_event& event)
{
    if (event.type == EV_SYN)
    {
        puts(event.code == SYN_DROPPED ? "mouse: dropped" : "mouse: syn");
    }
    else if (event.type == EV_KEY)
    {
        printf("mouse: key %u %d\n", static_cast<unsigned>(event.code), event.value);
    }
    else if (event.type == EV_REL && RelativeName(event.code) != nullptr)
    {
        printf("mouse: rel %s %d\n", RelativeName(event.code), event.value);
    }
    else if (event.type == EV_ABS && AbsoluteName(event.code) != nullptr)
    {
        printf("mouse: abs %s %d\n", AbsoluteName(event.code), event.value);
    }
    else
    {
        printf("mouse: type %u code %u value %d\n", static_cast<unsigned>(event.type),
               static_cast<unsigned>(event.code), event.value);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long count = DefaultCount;
    if (argc > 2 || (argc == 2 && (!FeatherOS::User::ParseUnsigned(argv[1], &count) || count == 0)))
    {
        puts("usage: mousetest [count]");
        return 1;
    }
    const int descriptor = open("/dev/mouse", O_RDONLY);
    if (descriptor < 0)
    {
        printf("mousetest: /dev/mouse: %s\n", strerror(errno));
        return 1;
    }
    puts("mousetest: ready");
    fflush(stdout);
    for (unsigned long long seen = 0; seen < count;)
    {
        input_event events[8];
        const ssize_t bytes = read(descriptor, events, sizeof(events));
        if (bytes < 0 || bytes % static_cast<ssize_t>(sizeof(input_event)) != 0)
        {
            printf("mousetest: read: %s\n", bytes < 0 ? strerror(errno) : "partial event");
            close(descriptor);
            return 1;
        }
        for (ssize_t index = 0;
             index < bytes / static_cast<ssize_t>(sizeof(input_event)) && seen < count;
             ++index, ++seen)
        {
            Print(events[index]);
        }
    }
    close(descriptor);
    return 0;
}
