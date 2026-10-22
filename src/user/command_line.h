#ifndef FEATHEROS_USER_COMMAND_LINE_H
#define FEATHEROS_USER_COMMAND_LINE_H

#include <stddef.h>
#include <string.h>

namespace FeatherOS::User
{

inline bool HelpRequested(int argc, char** argv)
{
    return argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0);
}

// A whole string of decimal digits that fits, unlike strtoull, which also takes spaces, signs,
// and trailing text.
inline bool ParseUnsigned(const char* text, unsigned long long* value)
{
    if (*text == '\0')
    {
        return false;
    }
    unsigned long long parsed = 0;
    for (; *text != '\0'; ++text)
    {
        if (*text < '0' || *text > '9')
        {
            return false;
        }
        const auto digit = static_cast<unsigned long long>(*text - '0');
        if (parsed > (~0ULL - digit) / 10)
        {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

enum class SplitResult : unsigned char
{
    Done,
    UnclosedQuote,
    TooManyWords,
};

// Splits line in place by the kernel's rules for a program's arguments: spaces separate words,
// and double quotes group them and are dropped. words gets a null pointer after the last word.
inline SplitResult SplitCommandLine(char* line, char** words, size_t capacity, size_t* count)
{
    *count = 0;
    char* output = line;
    const char* input = line;
    while (*input != '\0')
    {
        if (*input == ' ')
        {
            ++input;
            continue;
        }
        if (*count + 1 >= capacity)
        {
            return SplitResult::TooManyWords;
        }
        words[(*count)++] = output;
        bool quoted = false;
        while (*input != '\0' && (quoted || *input != ' '))
        {
            if (*input == '"')
            {
                quoted = !quoted;
            }
            else
            {
                *output++ = *input;
            }
            ++input;
        }
        if (quoted)
        {
            return SplitResult::UnclosedQuote;
        }
        // The word's source was at least as long, so this never overtakes the input.
        const bool at_end = *input == '\0';
        *output++ = '\0';
        if (!at_end)
        {
            ++input;
        }
    }
    words[*count] = nullptr;
    return SplitResult::Done;
}

} // namespace FeatherOS::User

#endif
