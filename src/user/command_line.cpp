#include "command_line.h"

#include <string.h>

namespace FeatherOS::User
{

bool HelpRequested(int argc, char** argv)
{
    return argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0);
}

bool ParseUnsigned(const char* text, unsigned long long* value)
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

SplitResult SplitCommandLine(char* line, char** words, size_t capacity, size_t* count, bool* quoted)
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
        words[*count] = output;
        bool inside_quotes = false;
        bool had_quote = false;
        while (*input != '\0' && (inside_quotes || *input != ' '))
        {
            if (*input == '"')
            {
                inside_quotes = !inside_quotes;
                had_quote = true;
            }
            else
            {
                *output++ = *input;
            }
            ++input;
        }
        if (inside_quotes)
        {
            return SplitResult::UnclosedQuote;
        }
        if (quoted != nullptr)
        {
            quoted[*count] = had_quote;
        }
        ++*count;
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

RedirectResult ExtractRedirects(char** words, const bool* quoted, size_t* count,
                                Redirect* redirects, size_t capacity, size_t* redirect_count)
{
    *redirect_count = 0;
    size_t kept = 0;
    for (size_t index = 0; index < *count; ++index)
    {
        char* word = words[index];
        RedirectKind kind = RedirectKind::Output;
        size_t operator_length = 0;
        if (!quoted[index])
        {
            if (strcmp(word, "2>&1") == 0)
            {
                kind = RedirectKind::ErrorToOutput;
                operator_length = 4;
            }
            else if (strncmp(word, "2>", 2) == 0)
            {
                kind = RedirectKind::Error;
                operator_length = 2;
            }
            else if (strncmp(word, ">>", 2) == 0)
            {
                kind = RedirectKind::Append;
                operator_length = 2;
            }
            else if (word[0] == '>' || word[0] == '<')
            {
                kind = word[0] == '>' ? RedirectKind::Output : RedirectKind::Input;
                operator_length = 1;
            }
        }
        if (operator_length == 0)
        {
            words[kept++] = word;
            continue;
        }
        if (*redirect_count == capacity)
        {
            return RedirectResult::TooMany;
        }
        const char* path = nullptr;
        if (kind != RedirectKind::ErrorToOutput)
        {
            path = word + operator_length;
            if (*path == '\0')
            {
                if (index + 1 == *count)
                {
                    return RedirectResult::MissingPath;
                }
                path = words[++index];
            }
        }
        redirects[(*redirect_count)++] = {kind, path};
    }
    *count = kept;
    words[kept] = nullptr;
    return RedirectResult::Done;
}

bool SeparatePipes(const char* line, char* output, size_t capacity)
{
    size_t length = 0;
    bool inside_quotes = false;
    for (; *line != '\0'; ++line)
    {
        const bool operator_character = *line == '|' && !inside_quotes;
        inside_quotes = *line == '"' ? !inside_quotes : inside_quotes;
        if (length + (operator_character ? 3 : 1) >= capacity)
        {
            return false;
        }
        if (operator_character)
        {
            output[length++] = ' ';
            output[length++] = '|';
            output[length++] = ' ';
            continue;
        }
        output[length++] = *line;
    }
    output[length] = '\0';
    return true;
}

PipelineResult SplitPipeline(char** words, const bool* quoted, size_t count, Stage* stages,
                             size_t capacity, size_t* stage_count)
{
    *stage_count = 0;
    size_t start = 0;
    for (size_t index = 0; index <= count; ++index)
    {
        const bool at_pipe = index < count && !quoted[index] && strcmp(words[index], "|") == 0;
        if (!at_pipe && index < count)
        {
            continue;
        }
        if (index == start)
        {
            return PipelineResult::EmptyStage;
        }
        if (*stage_count == capacity)
        {
            return PipelineResult::TooManyStages;
        }
        stages[(*stage_count)++] = {words + start, index - start, quoted + start};
        words[index] = nullptr;
        start = index + 1;
    }
    return PipelineResult::Done;
}

bool TakeBackgroundMarker(char** words, const bool* quoted, size_t* count)
{
    if (*count == 0 || quoted[*count - 1])
    {
        return false;
    }
    char* last = words[*count - 1];
    const size_t length = strlen(last);
    if (length == 0 || last[length - 1] != '&')
    {
        return false;
    }
    if (length == 1)
    {
        words[--*count] = nullptr;
        return true;
    }
    last[length - 1] = '\0';
    return true;
}

} // namespace FeatherOS::User
