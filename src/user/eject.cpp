#include "command_line.h"

#include <errno.h>
#include <featheros/disk.h>
#include <stdio.h>
#include <string.h>

constexpr char Usage[] = "usage: eject <disk>";
constexpr char Help[] =
    "usage: eject <disk>\n"
    "Unmounts every filesystem of a removable disk such as sda, writes out its\n"
    "data, flushes and stops the drive, so it can be unplugged. Nothing is\n"
    "unmounted if any of them is busy. Ejecting an ejected disk does nothing.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 2)
    {
        fprintf(stderr, "%s\n", Usage);
        return 1;
    }
    if (eject(argv[1]) == 0)
    {
        return 0;
    }
    fprintf(stderr, "eject: %s: %s\n", argv[1], errno == ENOENT ? "no such disk" : strerror(errno));
    return 1;
}
