#ifndef FEATHEROS_USER_CALCULATOR_H
#define FEATHEROS_USER_CALCULATOR_H

#include <stdint.h>

// Exact arithmetic on fractions, because user programs build without floating-point registers.
namespace FeatherOS::User
{

struct Rational
{
    int64_t numerator;
    // Always positive, and coprime with the numerator.
    int64_t denominator;
};

enum class CalculationError : uint8_t
{
    None,
    UnexpectedCharacter,
    UnexpectedEnd,
    DivisionByZero,
    Overflow,
};

struct Calculation
{
    CalculationError error;
    Rational value;
    // Where an UnexpectedCharacter error was found.
    uint32_t position;
};

namespace CalculatorDetail
{

// Larger denominators could overflow while FormatRational divides out decimal digits.
constexpr int64_t MaximumDenominator = 1000000000000000000 / 10;

uint64_t Magnitude(int64_t value);
int64_t GreatestCommonDivisor(int64_t left, int64_t right);
bool Make(int64_t numerator, int64_t denominator, Rational* result);
bool Add(const Rational& left, const Rational& right, Rational* result);
bool Multiply(const Rational& left, const Rational& right, Rational* result);

// Recursive descent over: expression = term (('+' | '-') term)*,
// term = factor (('*' | '/') factor)*, factor = ('+' | '-') factor | number | '(' expression ')'.
class Parser
{
public:
    explicit Parser(const char* text);
    Calculation Run();

private:
    const char* m_text;
    uint32_t m_position = 0;
    CalculationError m_error = CalculationError::None;

    char Peek();
    bool Fail(CalculationError error);
    bool Expression(Rational* value);
    bool Term(Rational* value);
    bool Factor(Rational* value);
    bool Number(Rational* value);
};

} // namespace CalculatorDetail

Calculation Calculate(const char* text);

// Sign, 19 integer digits, a point, six decimals, and a terminator.
constexpr uint32_t FormattedRationalLength = 28;

// Writes a decimal rounded to six places, without trailing zeros.
void FormatRational(const Rational& value, char (&buffer)[FormattedRationalLength]);

} // namespace FeatherOS::User

#endif
