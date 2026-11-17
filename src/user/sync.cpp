#include "command_line.h"

#include <stdio.h>
#include <unistd.h>

constexpr char Help[] = "usage: sync\n"
                        "Writes every mounted filesystem's pending changes to the disk.";

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
    sync();
    return 0;
}
