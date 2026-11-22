#include "command_line.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

constexpr char Help[] = "usage: readlink [-f] <path>\n"
                        "Prints the target a symbolic link holds. With -f, prints the path's\n"
                        "absolute form instead, with every link, . and .. resolved; it must\n"
                        "exist.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    const bool resolve = argc > 1 && strcmp(argv[1], "-f") == 0;
    const int operand = resolve ? 2 : 1;
    if (argc != operand + 1)
    {
        puts("usage: readlink [-f] <path>");
        return 1;
    }
    const char* path = argv[operand];
    char text[PATH_MAX];
    if (resolve)
    {
        if (realpath(path, text) == nullptr)
        {
            printf("readlink: %s: %s\n", path, strerror(errno));
            return 1;
        }
        puts(text);
        return 0;
    }
    const ssize_t length = readlink(path, text, sizeof(text) - 1);
    if (length < 0)
    {
        printf("readlink: %s: %s\n", path,
               errno == EINVAL ? "not a symbolic link" : strerror(errno));
        return 1;
    }
    text[length] = '\0';
    puts(text);
    return 0;
}
