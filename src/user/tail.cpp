#include "command_line.h"
#include "text_tools.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr char Help[] = "usage: tail [-n lines] [file]\n"
                        "Prints the last 10 lines, or -n lines, of a file or of its input.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long limit = 10;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0)
    {
        if (!FeatherOS::User::ParseUnsigned(argv[2], &limit) || limit > 100000)
        {
            puts("usage: tail [-n lines] [file]");
            return 1;
        }
        first = 3;
    }
    if (argc - first > 1)
    {
        puts("usage: tail [-n lines] [file]");
        return 1;
    }
    const char* name = argc > first ? argv[first] : nullptr;
    FILE* file = name == nullptr ? stdin : fopen(name, "r");
    if (file == nullptr)
    {
        printf("tail: %s: %s\n", name, strerror(errno));
        return 1;
    }
    FeatherOS::User::LastLines last(static_cast<size_t>(limit));
    char* line = nullptr;
    size_t capacity = 0;
    int status = 0;
    while (getline(&line, &capacity, file) >= 0)
    {
        if (!last.Add(line))
        {
            puts("tail: out of memory");
            status = 1;
            break;
        }
    }
    free(line);
    for (size_t index = 0; index < last.Count(); ++index)
    {
        fputs(last.Line(index), stdout);
    }
    if (file != stdin)
    {
        fclose(file);
    }
    return status;
}
