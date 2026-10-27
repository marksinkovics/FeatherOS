#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

constexpr char Help[] = "usage: touch <file>...\n"
                        "Creates each file that does not exist yet, empty. Times are not\n"
                        "changed, since files have none here.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv) || argc < 2)
    {
        puts(Help);
        return argc < 2 ? 1 : 0;
    }
    int status = 0;
    for (int index = 1; index < argc; ++index)
    {
        const int descriptor = open(argv[index], O_WRONLY | O_CREAT);
        if (descriptor < 0)
        {
            printf("touch: %s: %s\n", argv[index], strerror(errno));
            status = 1;
            continue;
        }
        close(descriptor);
    }
    return status;
}
