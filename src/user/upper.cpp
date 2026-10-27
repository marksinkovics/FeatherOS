#include "command_line.h"

#include <ctype.h>
#include <featheros.h>
#include <stdio.h>
#include <unistd.h>

constexpr char Help[] = "usage: upper\n"
                        "Reads one line at an upper> prompt and prints it in upper case. When\n"
                        "its input is a file or a pipe, it converts all of it instead.";

namespace
{

void PrintUpper(const char* text)
{
    for (; *text != '\0'; ++text)
    {
        putchar(toupper(static_cast<unsigned char>(*text)));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    char line[64];
    if (!isatty(STDIN_FILENO))
    {
        while (fgets(line, sizeof(line), stdin) != nullptr)
        {
            PrintUpper(line);
        }
        return ferror(stdin) ? 1 : 0;
    }
    if (featheros_read_line("upper> ", line, sizeof(line)) < 0)
    {
        puts("upper: could not read input");
        return 1;
    }
    PrintUpper(line);
    putchar('\n');
    return 0;
}
