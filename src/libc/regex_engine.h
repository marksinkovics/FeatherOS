#ifndef FEATHEROS_LIBC_REGEX_ENGINE_H
#define FEATHEROS_LIBC_REGEX_ENGINE_H

#include <stddef.h>

// POSIX regular expressions over bytes, basic and extended syntax, behind regex.h. A pattern
// compiles to a program of instructions. A Thompson simulation of it finds the leftmost-longest
// match, and a backtracking search inside that match finds the subexpressions.
namespace FeatherOS::LibC::Regex
{

// regex.h's REG_ codes have these values.
enum class Status : int
{
    Matched = 0,
    NoMatch = 1,
    BadPattern = 2,
    BadCollatingElement = 3,
    BadClass = 4,
    BadEscape = 5,
    BadReference = 6,
    UnmatchedBracket = 7,
    UnmatchedParenthesis = 8,
    UnmatchedBrace = 9,
    BadInterval = 10,
    BadRange = 11,
    NoMemory = 12,
    BadRepetition = 13,
};

// Compile flags with the values of REG_EXTENDED, REG_ICASE, REG_NEWLINE and REG_NOSUB.
constexpr int Extended = 1;
constexpr int IgnoreCase = 2;
constexpr int Newline = 4;
constexpr int NoSubexpressions = 8;
// Execute flags with the values of REG_NOTBOL and REG_NOTEOL.
constexpr int NotBeginning = 1;
constexpr int NotEnd = 2;

// The largest count an interval takes, RE_DUP_MAX.
constexpr int MaximumRepetition = 255;

struct Program;

// Byte offsets into the text, both -1 for a subexpression that took no part in the match.
struct Span
{
    long start;
    long end;
};

// On success, *program is to be given to Free.
Status Compile(const char* pattern, int flags, Program** program, size_t* subexpression_count);
// spans[0] is the whole match and spans[n] subexpression n, for count spans; those past the
// pattern's subexpressions are -1. With a count of 0 only whether there is a match is found.
Status Execute(const Program& program, const char* text, size_t length, int flags, Span* spans,
               size_t count);
void Free(Program* program);
const char* Describe(Status status);

} // namespace FeatherOS::LibC::Regex

#endif
