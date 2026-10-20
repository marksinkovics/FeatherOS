#include "calculator.h"
#include "process.h"
#include "runtime.h"

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
        WriteLine(text);
        return true;
    }
    case CalculationError::UnexpectedCharacter:
        WriteText("calc: unexpected '");
        WriteCharacter(static_cast<uint64_t>(expression[result.position]));
        WriteLine("'");
        return false;
    case CalculationError::UnexpectedEnd:
        WriteLine("calc: unexpected end of expression");
        return false;
    case CalculationError::DivisionByZero:
        WriteLine("calc: division by zero");
        return false;
    case CalculationError::Overflow:
        WriteLine("calc: number too large");
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

// calc [expression]: evaluates the expression, or reads one per line at a calc> prompt.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    if (arguments != nullptr && arguments->argument_count > 1)
    {
        const char* expression = SkipSpaces(arguments->command_line);
        while (*expression != ' ')
        {
            ++expression;
        }
        Exit(PrintCalculation(expression) ? 0 : 1);
    }

    WriteLine("Type an expression, or 'exit' to quit.");
    char line[MaximumCommandLineLength];
    for (;;)
    {
        if (IsSyscallError(ReadLine(line, sizeof(line), "calc> ")))
        {
            WriteLine("calc: could not read input");
            Exit(1);
        }
        const char* expression = SkipSpaces(line);
        if (StringEquals(expression, "exit") || StringEquals(expression, "quit"))
        {
            Exit(0);
        }
        if (*expression != '\0')
        {
            PrintCalculation(expression);
        }
    }
}
