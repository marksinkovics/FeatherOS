#include "command_line.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

constexpr char Help[] = "usage: rmdir <directory>\n"
                        "Removes an empty directory on the disk.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 2)
    {
        puts("usage: rmdir <directory>");
        return 1;
    }
    if (rmdir(argv[1]) != 0)
    {
        printf("rmdir: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
