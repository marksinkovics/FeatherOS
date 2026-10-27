#include "command_line.h"

#include <stdio.h>

constexpr char Help[] = "usage: clear\n"
                        "Clears the screen below the status bar and moves the cursor to its top.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 1)
    {
        puts(Help);
        return 1;
    }
    fputs("\x1b[H\x1b[2J", stdout);
    return 0;
}
