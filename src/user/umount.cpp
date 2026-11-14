#include "command_line.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

constexpr char Usage[] = "usage: umount <target>";
constexpr char Help[] = "usage: umount <target>\n"
                        "Unmounts the filesystem mounted at target, after writing out its data.\n"
                        "It fails while a file on it is open, a working directory lies inside it,\n"
                        "or another filesystem is mounted inside it.";

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
    if (umount(argv[1]) == 0)
    {
        return 0;
    }
    const char* reason = errno == EINVAL  ? "not mounted"
                         : errno == EBUSY ? "target is busy"
                                          : strerror(errno);
    fprintf(stderr, "umount: %s: %s\n", argv[1], reason);
    return 1;
}
