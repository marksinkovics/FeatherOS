#include "command_line.h"

#include <stdio.h>

constexpr char Help[] = "usage: echo <words...>\n"
                        "Prints its arguments separated by single spaces.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    for (int index = 1; index < argc; ++index)
    {
        printf(index > 1 ? " %s" : "%s", argv[index]);
    }
    putchar('\n');
    return 0;
}
