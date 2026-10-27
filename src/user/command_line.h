#ifndef FEATHEROS_USER_COMMAND_LINE_H
#define FEATHEROS_USER_COMMAND_LINE_H

#include <stddef.h>

namespace FeatherOS::User
{

bool HelpRequested(int argc, char** argv);

// A whole string of decimal digits that fits, unlike strtoull, which also takes spaces, signs,
// and trailing text.
bool ParseUnsigned(const char* text, unsigned long long* value);

enum class SplitResult : unsigned char
{
    Done,
    UnclosedQuote,
    TooManyWords,
};

// Splits line in place by the kernel's rules for a program's arguments: spaces separate words,
// and double quotes group them and are dropped. words gets a null pointer after the last word.
// If quoted is not null, quoted[i] says whether word i held a quote, so the shell can tell an
// operator such as > from a quoted ">".
SplitResult SplitCommandLine(char* line, char** words, size_t capacity, size_t* count,
                             bool* quoted = nullptr);

enum class RedirectKind : unsigned char
{
    // < file
    Input,
    // > file
    Output,
    // >> file
    Append,
    // 2> file
    Error,
    // 2>&1
    ErrorToOutput,
};

struct Redirect
{
    RedirectKind kind;
    // Null for 2>&1.
    const char* path;
};

enum class RedirectResult : unsigned char
{
    Done,
    MissingPath,
    TooMany,
};

// Takes the redirections out of words, in order, and closes the gap they leave. An operator is a
// word of its own or starts the word naming the file (">out.txt"); quoted words are never
// operators.
RedirectResult ExtractRedirects(char** words, const bool* quoted, size_t* count,
                                Redirect* redirects, size_t capacity, size_t* redirect_count);

// Copies line with spaces around each unquoted '|', so "ls|cat" splits like "ls | cat". Fails if
// the result does not fit.
bool SeparatePipes(const char* line, char* output, size_t capacity);

enum class PipelineResult : unsigned char
{
    Done,
    EmptyStage,
    TooManyStages,
};

struct Stage
{
    // A null pointer follows the stage's last word.
    char** words;
    size_t count;
    const bool* quoted;
};

// Cuts words at each unquoted "|" word, which becomes the null pointer ending its stage.
PipelineResult SplitPipeline(char** words, const bool* quoted, size_t count, Stage* stages,
                             size_t capacity, size_t* stage_count);

// Removes a trailing unquoted "&", as its own word or at the end of the last one ("sleep 9&"),
// and says whether there was one: the command then runs in the background.
bool TakeBackgroundMarker(char** words, const bool* quoted, size_t* count);

} // namespace FeatherOS::User

#endif
