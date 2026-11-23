#include "command_line.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

constexpr char Help[] = "usage: sync\n"
                        "Writes every mounted filesystem's pending changes to the disk.\n"
                        "It fails if one of them could not be written, and still syncs the others.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 1)
    {
        puts("usage: sync");
        return 1;
    }
    errno = 0;
    sync();
    if (errno != 0)
    {
        fprintf(stderr, "sync: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
