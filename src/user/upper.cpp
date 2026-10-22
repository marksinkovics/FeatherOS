#include "command_line.h"

#include <ctype.h>
#include <featheros.h>
#include <stdio.h>

constexpr char Help[] = "usage: upper\n"
                        "Reads one line at an upper> prompt and prints it in upper case.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    char line[64];
    if (featheros_read_line("upper> ", line, sizeof(line)) < 0)
    {
        puts("upper: could not read input");
        return 1;
    }
    for (char* character = line; *character != '\0'; ++character)
    {
        *character = static_cast<char>(toupper(static_cast<unsigned char>(*character)));
    }
    puts(line);
    return 0;
}
