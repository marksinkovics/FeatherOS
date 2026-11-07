#include "libc/regex_engine.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

namespace Regex = FeatherOS::LibC::Regex;

static_assert(REG_EXTENDED == Regex::Extended && REG_ICASE == Regex::IgnoreCase &&
              REG_NEWLINE == Regex::Newline && REG_NOSUB == Regex::NoSubexpressions);
static_assert(REG_NOTBOL == Regex::NotBeginning && REG_NOTEOL == Regex::NotEnd);
static_assert(REG_NOMATCH == static_cast<int>(Regex::Status::NoMatch) &&
              REG_BADRPT == static_cast<int>(Regex::Status::BadRepetition));
static_assert(RE_DUP_MAX == Regex::MaximumRepetition);

int regcomp(regex_t* regex, const char* pattern, int flags)
{
    Regex::Program* program = nullptr;
    size_t count = 0;
    const Regex::Status status = Regex::Compile(pattern, flags, &program, &count);
    if (status != Regex::Status::Matched)
    {
        return static_cast<int>(status);
    }
    regex->re_nsub = count;
    regex->__program = program;
    return 0;
}

int regexec(const regex_t* regex, const char* text, size_t count, regmatch_t* matches, int flags)
{
    const auto& program = *static_cast<const Regex::Program*>(regex->__program);
    Regex::Span* spans = nullptr;
    if (count > 0 && matches != nullptr)
    {
        spans = static_cast<Regex::Span*>(malloc(count * sizeof(Regex::Span)));
        if (spans == nullptr)
        {
            return REG_ESPACE;
        }
    }
    else
    {
        count = 0;
    }
    const Regex::Status status = Regex::Execute(program, text, strlen(text), flags, spans, count);
    if (status == Regex::Status::Matched)
    {
        for (size_t index = 0; index < count; ++index)
        {
            matches[index] = {spans[index].start, spans[index].end};
        }
    }
    free(spans);
    return static_cast<int>(status);
}

size_t regerror(int code, const regex_t*, char* buffer, size_t size)
{
    const char* message = Regex::Describe(static_cast<Regex::Status>(code));
    const size_t length = strlen(message);
    if (size > 0)
    {
        const size_t copied = length < size - 1 ? length : size - 1;
        memcpy(buffer, message, copied);
        buffer[copied] = '\0';
    }
    return length + 1;
}

void regfree(regex_t* regex)
{
    Regex::Free(static_cast<Regex::Program*>(regex->__program));
    regex->__program = nullptr;
}
