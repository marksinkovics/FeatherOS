#include "command_line.h"

#include <errno.h>
#include <stdio.h>

constexpr char Help[] = "usage: cat <file>\n"
                        "Prints a file. A relative name that is missing is also tried in\n"
                        "/system, so cat hello.txt works from any directory.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 2)
    {
        puts("usage: cat <file>");
        return 1;
    }
    const char* name = argv[1];
    FILE* file = fopen(name, "r");
    if (file == nullptr && errno == ENOENT && name[0] != '/')
    {
        char system_path[96];
        snprintf(system_path, sizeof(system_path), "/system/%s", name);
        file = fopen(system_path, "r");
    }
    if (file == nullptr)
    {
        printf("cat: cannot open %s\n", name);
        return 1;
    }
    // The console shows tabs as spaces and drops other control characters.
    int last = '\n';
    for (int character; (character = fgetc(file)) != EOF;)
    {
        putchar(character);
        last = character;
    }
    if (ferror(file))
    {
        puts("cat: read failed");
        return 1;
    }
    if (last != '\n')
    {
        putchar('\n');
    }
    fclose(file);
    return 0;
}
