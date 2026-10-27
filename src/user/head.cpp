#include "command_line.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr char Help[] = "usage: head [-n lines] [file...]\n"
                        "Prints the first 10 lines, or -n lines, of each file or of its input.";

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
        if (!FeatherOS::User::ParseUnsigned(argv[2], &limit))
        {
            puts("usage: head [-n lines] [file...]");
            return 1;
        }
        first = 3;
    }
    int status = 0;
    const int files = argc - first;
    for (int index = first; index < argc || (files == 0 && index == first); ++index)
    {
        const char* name = files == 0 ? nullptr : argv[index];
        FILE* file = name == nullptr ? stdin : fopen(name, "r");
        if (file == nullptr)
        {
            printf("head: %s: %s\n", name, strerror(errno));
            status = 1;
            continue;
        }
        if (files > 1)
        {
            printf("%s==> %s <==\n", index == first ? "" : "\n", name);
        }
        char* line = nullptr;
        size_t capacity = 0;
        for (unsigned long long count = 0; count < limit && getline(&line, &capacity, file) >= 0;
             ++count)
        {
            fputs(line, stdout);
        }
        free(line);
        if (file != stdin)
        {
            fclose(file);
        }
    }
    return status;
}
