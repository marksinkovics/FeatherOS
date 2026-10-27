#include "command_line.h"
#include "text_tools.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr char Help[] = "usage: sort [-rnu] [file...]\n"
                        "Prints the lines of the files, or of its input, in order: -r reverses\n"
                        "it, -n orders by each line's leading number, -u drops repeated lines.";

namespace
{

bool reverse = false;
bool numeric = false;

int CompareEntries(const void* left, const void* right)
{
    const int order = FeatherOS::User::CompareLines(*static_cast<char* const*>(left),
                                                    *static_cast<char* const*>(right), numeric);
    return reverse ? -order : order;
}

// Reads every line of file into lines, growing it as needed; false when out of memory.
bool ReadLines(FILE* file, char*** lines, size_t* count, size_t* capacity)
{
    char* line = nullptr;
    size_t line_capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &line_capacity, file)) >= 0)
    {
        if (length > 0 && line[length - 1] == '\n')
        {
            line[length - 1] = '\0';
        }
        if (*count == *capacity)
        {
            const size_t grown = *capacity == 0 ? 64 : *capacity * 2;
            auto* larger = static_cast<char**>(realloc(*lines, grown * sizeof(char*)));
            if (larger == nullptr)
            {
                free(line);
                return false;
            }
            *lines = larger;
            *capacity = grown;
        }
        (*lines)[(*count)++] = strdup(line);
        if ((*lines)[*count - 1] == nullptr)
        {
            free(line);
            return false;
        }
    }
    free(line);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    bool letters[26] = {};
    char bad = 0;
    const int first = FeatherOS::User::ParseFlags(argc, argv, "rnu", letters, &bad);
    if (first < 0)
    {
        printf("sort: unknown option -%c\n", bad);
        return 1;
    }
    reverse = letters['r' - 'a'];
    numeric = letters['n' - 'a'];
    const bool unique = letters['u' - 'a'];
    char** lines = nullptr;
    size_t count = 0;
    size_t capacity = 0;
    int status = 0;
    const int files = argc - first;
    for (int index = first; index < argc || (files == 0 && index == first); ++index)
    {
        const char* name = files == 0 ? nullptr : argv[index];
        FILE* file = name == nullptr ? stdin : fopen(name, "r");
        if (file == nullptr)
        {
            printf("sort: %s: %s\n", name, strerror(errno));
            status = 1;
            continue;
        }
        const bool read = ReadLines(file, &lines, &count, &capacity);
        if (file != stdin)
        {
            fclose(file);
        }
        if (!read)
        {
            puts("sort: out of memory");
            return 1;
        }
    }
    qsort(lines, count, sizeof(char*), CompareEntries);
    for (size_t index = 0; index < count; ++index)
    {
        if (!unique || index == 0 || CompareEntries(&lines[index - 1], &lines[index]) != 0)
        {
            puts(lines[index]);
        }
        free(lines[index]);
    }
    free(lines);
    return status;
}
