#include "calculator.h"
#include "command_line.h"

#include <featheros.h>
#include <stdio.h>
#include <string.h>

using namespace FeatherOS::User;

namespace
{

bool PrintCalculation(const char* expression)
{
    const Calculation result = Calculate(expression);
    switch (result.error)
    {
    case CalculationError::None:
    {
        char text[FormattedRationalLength];
        FormatRational(result.value, text);
        puts(text);
        return true;
    }
    case CalculationError::UnexpectedCharacter:
        printf("calc: unexpected '%c'\n", expression[result.position]);
        return false;
    case CalculationError::UnexpectedEnd:
        puts("calc: unexpected end of expression");
        return false;
    case CalculationError::DivisionByZero:
        puts("calc: division by zero");
        return false;
    case CalculationError::Overflow:
        puts("calc: number too large");
        return false;
    }
    return false;
}

const char* SkipSpaces(const char* text)
{
    while (*text == ' ')
    {
        ++text;
    }
    return text;
}

} // namespace

constexpr char Help[] =
    "usage: calc [expression]\n"
    "Evaluates + - * / with parentheses and decimals, e.g. calc 4 + 66 - (3 / 4).\n"
    "Without an expression, reads one per line at a calc> prompt; type exit to quit.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    char line[64];
    if (argc > 1)
    {
        size_t length = 0;
        for (int index = 1; index < argc && length < sizeof(line); ++index)
        {
            length += static_cast<size_t>(snprintf(line + length, sizeof(line) - length,
                                                   index > 1 ? " %s" : "%s", argv[index]));
        }
        return PrintCalculation(line) ? 0 : 1;
    }

    puts("Type an expression, or 'exit' to quit.");
    for (;;)
    {
        if (featheros_read_line("calc> ", line, sizeof(line)) < 0)
        {
            puts("calc: could not read input");
            return 1;
        }
        const char* expression = SkipSpaces(line);
        if (strcmp(expression, "exit") == 0 || strcmp(expression, "quit") == 0)
        {
            return 0;
        }
        if (*expression != '\0')
        {
            PrintCalculation(expression);
        }
    }
}
