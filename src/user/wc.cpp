#include "command_line.h"
#include "text_tools.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

using FeatherOS::User::TextCounts;

constexpr char Help[] = "usage: wc [-lwc] [file...]\n"
                        "Counts the lines, words and bytes of each file or of its input; -l, -w\n"
                        "and -c pick which. Several files also get a total.";

namespace
{

bool show_lines = true;
bool show_words = true;
bool show_bytes = true;

void PrintCounts(const TextCounts& counts, const char* name)
{
    const char* separator = "";
    const auto field = [&](bool shown, unsigned long value)
    {
        if (shown)
        {
            printf("%s%lu", separator, value);
            separator = " ";
        }
    };
    field(show_lines, counts.lines);
    field(show_words, counts.words);
    field(show_bytes, counts.bytes);
    printf(name != nullptr ? " %s\n" : "\n", name);
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
    const int first = FeatherOS::User::ParseFlags(argc, argv, "lwc", letters, &bad);
    if (first < 0)
    {
        printf("wc: unknown option -%c\n", bad);
        return 1;
    }
    if (letters['l' - 'a'] || letters['w' - 'a'] || letters['c' - 'a'])
    {
        show_lines = letters['l' - 'a'];
        show_words = letters['w' - 'a'];
        show_bytes = letters['c' - 'a'];
    }
    TextCounts total = {0, 0, 0};
    int status = 0;
    const int files = argc - first;
    for (int index = first; index < argc || (files == 0 && index == first); ++index)
    {
        const char* name = files == 0 ? nullptr : argv[index];
        FILE* file = name == nullptr ? stdin : fopen(name, "r");
        if (file == nullptr)
        {
            printf("wc: %s: %s\n", name, strerror(errno));
            status = 1;
            continue;
        }
        TextCounts counts = {0, 0, 0};
        bool in_word = false;
        char buffer[512];
        for (size_t count; (count = fread(buffer, 1, sizeof(buffer), file)) > 0;)
        {
            FeatherOS::User::CountText(buffer, count, &counts, &in_word);
        }
        PrintCounts(counts, name);
        total.lines += counts.lines;
        total.words += counts.words;
        total.bytes += counts.bytes;
        if (file != stdin)
        {
            fclose(file);
        }
    }
    if (files > 1)
    {
        PrintCounts(total, "total");
    }
    return status;
}
