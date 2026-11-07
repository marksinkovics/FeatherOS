#include "command_line.h"
#include "text_tools.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr char Usage[] = "usage: grep [-EFivnco] <pattern> [file...]";
constexpr char Help[] =
    "usage: grep [-EFivnco] <pattern> [file...]\n"
    "Prints the lines that match a basic regular expression, from each file or\n"
    "its input: -E takes an extended one and -F plain text, -i ignores case,\n"
    "-v prints the other lines, -n numbers them, -c only counts, -o prints only\n"
    "the matched parts. Exits with 0 when a line matched, 1 when none did, 2 on\n"
    "an error.";

namespace
{

struct Options
{
    bool invert;
    bool numbered;
    bool count_only;
    bool only_matching;
    // Set when more than one file is searched, to name each.
    const char* name;
};

void PrintPrefix(const Options& options, unsigned long number)
{
    if (options.name != nullptr)
    {
        printf("%s:", options.name);
    }
    if (options.numbered)
    {
        printf("%lu:", number);
    }
}

// Prints each non-empty match in line, as GNU grep -o does.
bool PrintMatches(const regex_t& regex, const char* line, const Options& options,
                  unsigned long number)
{
    const char* cursor = line;
    regmatch_t match;
    int flags = 0;
    while (*cursor != '\0')
    {
        const int status = regexec(&regex, cursor, 1, &match, flags);
        if (status == REG_NOMATCH)
        {
            break;
        }
        if (status != 0)
        {
            return false;
        }
        if (match.rm_eo > match.rm_so)
        {
            PrintPrefix(options, number);
            printf("%.*s\n", static_cast<int>(match.rm_eo - match.rm_so), cursor + match.rm_so);
        }
        cursor += match.rm_eo > match.rm_so ? match.rm_eo : match.rm_so + 1;
        flags = REG_NOTBOL;
    }
    return true;
}

// The number of lines selected, or -1 when matching failed.
long Search(const regex_t& regex, FILE* file, const Options& options)
{
    char* line = nullptr;
    size_t capacity = 0;
    unsigned long number = 0;
    long selected = 0;
    ssize_t length = 0;
    while ((length = getline(&line, &capacity, file)) >= 0)
    {
        ++number;
        if (length > 0 && line[length - 1] == '\n')
        {
            line[length - 1] = '\0';
        }
        const int status = regexec(&regex, line, 0, nullptr, 0);
        if (status != 0 && status != REG_NOMATCH)
        {
            selected = -1;
            break;
        }
        if ((status == 0) == options.invert)
        {
            continue;
        }
        ++selected;
        if (options.count_only)
        {
            continue;
        }
        if (!options.only_matching)
        {
            PrintPrefix(options, number);
            puts(line);
        }
        else if (!options.invert && !PrintMatches(regex, line, options, number))
        {
            selected = -1;
            break;
        }
    }
    free(line);
    return selected;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    bool letters[128] = {};
    char bad = 0;
    const int first = FeatherOS::User::ParseFlags(argc, argv, "EFivnco", letters, &bad);
    if (first < 0 || first >= argc)
    {
        if (first < 0)
        {
            printf("grep: unknown option -%c\n", bad);
        }
        puts(Usage);
        return 2;
    }
    if (letters['E'] && letters['F'])
    {
        puts("grep: -E and -F do not go together");
        return 2;
    }
    char* fixed = nullptr;
    const char* pattern = argv[first];
    if (letters['F'])
    {
        fixed = FeatherOS::User::FixedTextPattern(pattern);
        if (fixed == nullptr)
        {
            puts("grep: out of memory");
            return 2;
        }
        pattern = fixed;
    }
    const bool only_matching = letters['o'];
    const int flags = (letters['E'] ? REG_EXTENDED : 0) | (letters['i'] ? REG_ICASE : 0) |
                      (only_matching ? 0 : REG_NOSUB);
    regex_t regex;
    const int status = regcomp(&regex, pattern, flags);
    free(fixed);
    if (status != 0)
    {
        char message[64];
        regerror(status, &regex, message, sizeof(message));
        printf("grep: %s\n", message);
        return 2;
    }

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
        const Options options = {letters['v'], letters['n'], letters['c'], only_matching,
                                 files > 1 ? name : nullptr};
        const long selected = Search(regex, file, options);
        if (file != stdin)
        {
            fclose(file);
        }
        if (selected < 0)
        {
            puts("grep: the pattern took too much memory to match");
            failed = true;
            break;
        }
        if (options.count_only)
        {
            printf(options.name != nullptr ? "%s:%ld\n" : "%.0s%ld\n",
                   options.name != nullptr ? options.name : "", selected);
        }
        matched_any = matched_any || selected > 0;
    }
    regfree(&regex);
    return failed ? 2 : matched_any ? 0 : 1;
}
