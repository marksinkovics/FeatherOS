#include "command_line.h"
#include "text_tools.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr char Help[] =
    "usage: grep [-ivnc] <text> [file...]\n"
    "Prints the lines that contain text, from each file or its input: -i\n"
    "ignores case, -v prints the other lines, -n numbers them, -c only counts.\n"
    "Exits with 0 when a line matched, 1 when none did, 2 on an error.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    bool letters[26] = {};
    char bad = 0;
    const int first = FeatherOS::User::ParseFlags(argc, argv, "ivnc", letters, &bad);
    if (first < 0 || first >= argc)
    {
        if (first < 0)
        {
            printf("grep: unknown option -%c\n", bad);
        }
        puts("usage: grep [-ivnc] <text> [file...]");
        return 2;
    }
    const char* pattern = argv[first];
    const bool ignore_case = letters['i' - 'a'];
    const bool invert = letters['v' - 'a'];
    const bool numbered = letters['n' - 'a'];
    const bool count_only = letters['c' - 'a'];
    const int files = argc - first - 1;
    bool matched_any = false;
    bool failed = false;
    for (int index = first + 1; index < argc || (files == 0 && index == first + 1); ++index)
    {
        const char* name = files == 0 ? nullptr : argv[index];
        FILE* file = name == nullptr ? stdin : fopen(name, "r");
        if (file == nullptr)
        {
            printf("grep: %s: %s\n", name, strerror(errno));
            failed = true;
            continue;
        }
        char* line = nullptr;
        size_t capacity = 0;
        unsigned long number = 0;
        unsigned long matches = 0;
        while (getline(&line, &capacity, file) >= 0)
        {
            ++number;
            if (FeatherOS::User::ContainsText(line, pattern, ignore_case) == invert)
            {
                continue;
            }
            ++matches;
            if (count_only)
            {
                continue;
            }
            if (files > 1)
            {
                printf("%s:", name);
            }
            if (numbered)
            {
                printf("%lu:", number);
            }
            const size_t length = strlen(line);
            fputs(line, stdout);
            if (length == 0 || line[length - 1] != '\n')
            {
                putchar('\n');
            }
        }
        free(line);
        if (count_only)
        {
            printf(files > 1 ? "%s:%lu\n" : "%.0s%lu\n", files > 1 ? name : "", matches);
        }
        matched_any = matched_any || matches > 0;
        if (file != stdin)
        {
            fclose(file);
        }
    }
    return failed ? 2 : matched_any ? 0 : 1;
}
