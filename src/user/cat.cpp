#include "command_line.h"

#include <errno.h>
#include <stdio.h>

constexpr char Help[] = "usage: cat [file]\n"
                        "Prints a file, or its input without one. A relative name that is\n"
                        "missing is also tried in /system, so cat hello.txt works anywhere.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc > 2)
    {
        puts("usage: cat [file]");
        return 1;
    }
    const char* name = argc == 2 ? argv[1] : "standard input";
    FILE* file = argc == 2 ? fopen(name, "r") : stdin;
    if (file == nullptr && errno == ENOENT && argc == 2 && name[0] != '/')
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
    if (file != stdin)
    {
        fclose(file);
    }
    return 0;
}
