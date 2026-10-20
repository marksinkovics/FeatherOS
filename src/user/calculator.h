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

inline uint64_t Magnitude(int64_t value)
{
    return value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}

inline int64_t GreatestCommonDivisor(int64_t left, int64_t right)
{
    uint64_t a = Magnitude(left);
    uint64_t b = Magnitude(right);
    while (b != 0)
    {
        const uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a == 0 ? 1 : static_cast<int64_t>(a);
}

inline bool Make(int64_t numerator, int64_t denominator, Rational* result)
{
    if (numerator == INT64_MIN || denominator == INT64_MIN || denominator == 0)
    {
        return false;
    }
    if (denominator < 0)
    {
        numerator = -numerator;
        denominator = -denominator;
    }
    const int64_t divisor = GreatestCommonDivisor(numerator, denominator);
    result->numerator = numerator / divisor;
    result->denominator = denominator / divisor;
    return result->denominator <= MaximumDenominator;
}

inline bool Add(const Rational& left, const Rational& right, Rational* result)
{
    const int64_t divisor = GreatestCommonDivisor(left.denominator, right.denominator);
    int64_t left_scaled;
    int64_t right_scaled;
    int64_t numerator;
    int64_t denominator;
    if (__builtin_mul_overflow(left.numerator, right.denominator / divisor, &left_scaled) ||
        __builtin_mul_overflow(right.numerator, left.denominator / divisor, &right_scaled) ||
        __builtin_add_overflow(left_scaled, right_scaled, &numerator) ||
        __builtin_mul_overflow(left.denominator, right.denominator / divisor, &denominator))
    {
        return false;
    }
    return Make(numerator, denominator, result);
}

inline bool Multiply(const Rational& left, const Rational& right, Rational* result)
{
    // Cancelling across the operands first keeps the products small.
    const int64_t first = GreatestCommonDivisor(left.numerator, right.denominator);
    const int64_t second = GreatestCommonDivisor(right.numerator, left.denominator);
    int64_t numerator;
    int64_t denominator;
    if (__builtin_mul_overflow(left.numerator / first, right.numerator / second, &numerator) ||
        __builtin_mul_overflow(left.denominator / second, right.denominator / first, &denominator))
    {
        return false;
    }
    return Make(numerator, denominator, result);
}

// Recursive descent over: expression = term (('+' | '-') term)*,
// term = factor (('*' | '/') factor)*, factor = ('+' | '-') factor | number | '(' expression ')'.
class Parser
{
public:
    explicit Parser(const char* text) : m_text(text) {}

    Calculation Run()
    {
        Rational value = {0, 1};
        if (Expression(&value) && Peek() != '\0')
        {
            Fail(CalculationError::UnexpectedCharacter);
        }
        return {m_error, m_error == CalculationError::None ? value : Rational{0, 1}, m_position};
    }

private:
    const char* m_text;
    uint32_t m_position = 0;
    CalculationError m_error = CalculationError::None;

    char Peek()
    {
        while (m_text[m_position] == ' ')
        {
            ++m_position;
        }
        return m_text[m_position];
    }

    bool Fail(CalculationError error)
    {
        if (error == CalculationError::UnexpectedCharacter && m_text[m_position] == '\0')
        {
            error = CalculationError::UnexpectedEnd;
        }
        m_error = error;
        return false;
    }

    bool Expression(Rational* value)
    {
        if (!Term(value))
        {
            return false;
        }
        for (char operation = Peek(); operation == '+' || operation == '-'; operation = Peek())
        {
            ++m_position;
            Rational right;
            if (!Term(&right))
            {
                return false;
            }
            if (operation == '-')
            {
                right.numerator = -right.numerator;
            }
            if (!Add(*value, right, value))
            {
                return Fail(CalculationError::Overflow);
            }
        }
        return true;
    }

    bool Term(Rational* value)
    {
        if (!Factor(value))
        {
            return false;
        }
        for (char operation = Peek(); operation == '*' || operation == '/'; operation = Peek())
        {
            ++m_position;
            Rational right;
            if (!Factor(&right))
            {
                return false;
            }
            if (operation == '/')
            {
                if (right.numerator == 0)
                {
                    return Fail(CalculationError::DivisionByZero);
                }
                right = right.numerator < 0 ? Rational{-right.denominator, -right.numerator}
                                            : Rational{right.denominator, right.numerator};
            }
            if (!Multiply(*value, right, value))
            {
                return Fail(CalculationError::Overflow);
            }
        }
        return true;
    }

    bool Factor(Rational* value)
    {
        const char next = Peek();
        if (next == '+' || next == '-')
        {
            ++m_position;
            if (!Factor(value))
            {
                return false;
            }
            if (next == '-')
            {
                value->numerator = -value->numerator;
            }
            return true;
        }
        if (next == '(')
        {
            ++m_position;
            if (!Expression(value))
            {
                return false;
            }
            if (Peek() != ')')
            {
                return Fail(CalculationError::UnexpectedCharacter);
            }
            ++m_position;
            return true;
        }
        return Number(value);
    }

    bool Number(Rational* value)
    {
        int64_t numerator = 0;
        int64_t denominator = 1;
        bool digits = false;
        bool fraction = false;
        for (;; ++m_position)
        {
            const char character = m_text[m_position];
            if (character == '.' && !fraction)
            {
                fraction = true;
                continue;
            }
            if (character < '0' || character > '9')
            {
                break;
            }
            digits = true;
            if (__builtin_mul_overflow(numerator, 10, &numerator) ||
                __builtin_add_overflow(numerator, character - '0', &numerator) ||
                (fraction && __builtin_mul_overflow(denominator, 10, &denominator)))
            {
                return Fail(CalculationError::Overflow);
            }
        }
        if (!digits)
        {
            return Fail(CalculationError::UnexpectedCharacter);
        }
        return Make(numerator, denominator, value) || Fail(CalculationError::Overflow);
    }
};

} // namespace CalculatorDetail

inline Calculation Calculate(const char* text)
{
    return CalculatorDetail::Parser(text).Run();
}

// Sign, 19 integer digits, a point, six decimals, and a terminator.
constexpr uint32_t FormattedRationalLength = 28;

// Writes a decimal rounded to six places, without trailing zeros.
inline void FormatRational(const Rational& value, char (&buffer)[FormattedRationalLength])
{
    constexpr uint32_t Places = 6;
    const uint64_t magnitude = CalculatorDetail::Magnitude(value.numerator);
    const auto denominator = static_cast<uint64_t>(value.denominator);
    uint64_t integer = magnitude / denominator;
    uint64_t remainder = magnitude % denominator;
    uint64_t decimals = 0;
    for (uint32_t place = 0; place < Places; ++place)
    {
        remainder *= 10;
        decimals = decimals * 10 + remainder / denominator;
        remainder %= denominator;
    }
    // Round half away from zero.
    if (remainder * 2 >= denominator)
    {
        ++decimals;
        if (decimals == 1000000)
        {
            decimals = 0;
            ++integer;
        }
    }

    uint32_t length = 0;
    if (value.numerator < 0 && (integer != 0 || decimals != 0))
    {
        buffer[length++] = '-';
    }
    char digits[20];
    uint32_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + integer % 10);
        integer /= 10;
    } while (integer != 0);
    while (count > 0)
    {
        buffer[length++] = digits[--count];
    }
    if (decimals != 0)
    {
        uint32_t places = Places;
        while (decimals % 10 == 0)
        {
            decimals /= 10;
            --places;
        }
        buffer[length++] = '.';
        for (uint32_t place = places; place > 0; --place)
        {
            uint64_t digit = decimals;
            for (uint32_t shift = 1; shift < place; ++shift)
            {
                digit /= 10;
            }
            buffer[length++] = static_cast<char>('0' + digit % 10);
        }
    }
    buffer[length] = '\0';
}

} // namespace FeatherOS::User

#endif
