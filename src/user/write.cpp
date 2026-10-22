#include "command_line.h"

#include <stdio.h>
#include <string.h>

constexpr char Help[] = "usage: write [-a] <file> <text...>\n"
                        "Writes the text and a newline to a file on the disk, replacing it;\n"
                        "-a appends instead.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    const bool append = argc > 1 && strcmp(argv[1], "-a") == 0;
    const int name_index = append ? 2 : 1;
    if (argc < name_index + 2)
    {
        puts("usage: write [-a] <file> <text...>");
        return 1;
    }
    FILE* file = fopen(argv[name_index], append ? "a" : "w");
    if (file == nullptr)
    {
        printf("write: cannot open %s\n", argv[name_index]);
        return 1;
    }
    for (int index = name_index + 1; index < argc; ++index)
    {
        fputs(argv[index], file);
        fputc(index + 1 < argc ? ' ' : '\n', file);
    }
    const bool failed = ferror(file) != 0;
    if (fclose(file) != 0 || failed)
    {
        puts("write: the disk is full");
        return 1;
    }
    return 0;
}
