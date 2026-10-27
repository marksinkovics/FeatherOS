#include "command_line.h"
#include "file_copy.h"

#include <limits.h>
#include <stdio.h>

constexpr char Help[] = "usage: cp <source> <target>\n"
                        "Copies a file. A target that is a directory gets a file of the\n"
                        "source's name; an existing file is replaced.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 3)
    {
        puts("usage: cp <source> <target>");
        return 1;
    }
    char target[2 * PATH_MAX];
    return FeatherOS::User::ResolveTarget("cp", argv[1], argv[2], target, sizeof(target)) &&
                   FeatherOS::User::CopyFile("cp", argv[1], target)
               ? 0
               : 1;
}
