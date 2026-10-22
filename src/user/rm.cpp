#include "command_line.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

constexpr char Help[] = "usage: rm <file>\n"
                        "Deletes a file on the disk.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 2)
    {
        puts("usage: rm <file>");
        return 1;
    }
    if (unlink(argv[1]) == 0)
    {
        return 0;
    }
    printf("rm: %s%s\n", argv[1],
           errno == ENOENT  ? ": no such file"
           : errno == EBUSY ? ": the file is open"
                            : ": cannot remove");
    return 1;
}
