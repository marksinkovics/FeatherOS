#include "command_line.h"
#include "file_copy.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

constexpr char Help[] = "usage: ln [-s] <target> <link>\n"
                        "Makes link a second name for target, or with -s a symbolic link that\n"
                        "holds target as given, which need not exist and is resolved from the\n"
                        "link's directory. A link that names a directory gets target's own\n"
                        "name inside it. /tmp and /etc hold symbolic links but not hard links;\n"
                        "the disk holds neither.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    const bool symbolic = argc > 1 && strcmp(argv[1], "-s") == 0;
    const int first_operand = symbolic ? 2 : 1;
    if (argc != first_operand + 2)
    {
        puts("usage: ln [-s] <target> <link>");
        return 1;
    }
    const char* target = argv[first_operand];
    char link_path[2 * PATH_MAX];
    if (!FeatherOS::User::ResolveTarget("ln", target, argv[first_operand + 1], link_path,
                                        sizeof(link_path)))
    {
        return 1;
    }
    if ((symbolic ? symlink(target, link_path) : link(target, link_path)) != 0)
    {
        printf("ln: %s: %s\n", link_path, strerror(errno));
        return 1;
    }
    return 0;
}
