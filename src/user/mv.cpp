#include "command_line.h"
#include "file_copy.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr char Help[] = "usage: mv <source> <target>\n"
                        "Moves or renames a file or directory. A target that is a directory\n"
                        "gets the source under its own name; an existing file is replaced.\n"
                        "Between the disk and /tmp a file is copied, then removed.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 3)
    {
        puts("usage: mv <source> <target>");
        return 1;
    }
    const char* source = argv[1];
    char target[2 * PATH_MAX];
    if (!FeatherOS::User::ResolveTarget("mv", source, argv[2], target, sizeof(target)))
    {
        return 1;
    }
    if (rename(source, target) == 0)
    {
        return 0;
    }
    struct stat status;
    if (errno != EXDEV || stat(source, &status) != 0 || S_ISDIR(status.st_mode))
    {
        printf("mv: %s: %s\n", source, strerror(errno));
        return 1;
    }
    if (!FeatherOS::User::CopyFile("mv", source, target))
    {
        return 1;
    }
    if (unlink(source) != 0)
    {
        printf("mv: %s: %s\n", source, strerror(errno));
        return 1;
    }
    return 0;
}
