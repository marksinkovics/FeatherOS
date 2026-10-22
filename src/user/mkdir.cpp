#include "command_line.h"

#include <stdio.h>
#include <sys/stat.h>

constexpr char Help[] = "usage: mkdir <directory>\n"
                        "Creates a directory on the disk.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 2)
    {
        puts("usage: mkdir <directory>");
        return 1;
    }
    if (mkdir(argv[1], 0777) != 0)
    {
        printf("mkdir: %s: cannot create the directory\n", argv[1]);
        return 1;
    }
    return 0;
}
