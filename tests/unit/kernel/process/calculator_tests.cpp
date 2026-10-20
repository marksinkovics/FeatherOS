#include <catch2/catch_test_macros.hpp>

#include "user/calculator.h"

#include <string>

namespace
{

using FeatherOS::User::CalculationError;

std::string Result(const char* expression)
{
    const FeatherOS::User::Calculation calculation = FeatherOS::User::Calculate(expression);
    REQUIRE(calculation.error == CalculationError::None);
    char text[FeatherOS::User::FormattedRationalLength];
    FeatherOS::User::FormatRational(calculation.value, text);
    return text;
}

CalculationError Error(const char* expression)
{
    return FeatherOS::User::Calculate(expression).error;
}

} // namespace

TEST_CASE("calculator follows precedence, parentheses, and unary signs")
{
    REQUIRE(Result("4 + 66 - (3 / 4)") == "69.25");
    REQUIRE(Result("1 + 2 - 3") == "0");
    REQUIRE(Result("2 + 3 * 4") == "14");
    REQUIRE(Result("(2 + 3) * 4") == "20");
    REQUIRE(Result("8 / 2 / 2") == "2");
    REQUIRE(Result("10 - 2 - 3") == "5");
    REQUIRE(Result("-3 * -(2 + 1)") == "9");
    REQUIRE(Result("+5") == "5");
    REQUIRE(Result("1+2*3") == "7");
    REQUIRE(Result("  7  ") == "7");
}

TEST_CASE("calculator keeps fractions exact and rounds to six places")
{
    REQUIRE(Result("1 / 3") == "0.333333");
    REQUIRE(Result("2 / 3") == "0.666667");
    REQUIRE(Result("1 / 3 * 3") == "1");
    REQUIRE(Result("0.1 + 0.2") == "0.3");
    REQUIRE(Result("-1 / 8") == "-0.125");
    REQUIRE(Result("-1 / 10000000") == "0");
    REQUIRE(Result("0.9999999") == "1");
    REQUIRE(Result("9223372036854775807") == "9223372036854775807");
}

TEST_CASE("calculator reports malformed expressions and arithmetic errors")
{
    REQUIRE(Error("") == CalculationError::UnexpectedEnd);
    REQUIRE(Error("1 +") == CalculationError::UnexpectedEnd);
    REQUIRE(Error("(1 + 2") == CalculationError::UnexpectedEnd);
    REQUIRE(Error("1 2") == CalculationError::UnexpectedCharacter);
    REQUIRE(Error("1 + x") == CalculationError::UnexpectedCharacter);
    REQUIRE(Error("1.2.3") == CalculationError::UnexpectedCharacter);
    REQUIRE(FeatherOS::User::Calculate("2 * ) 1").position == 4);
    REQUIRE(Error("7 / 0") == CalculationError::DivisionByZero);
    REQUIRE(Error("1 / (2 - 2)") == CalculationError::DivisionByZero);
    REQUIRE(Error("9223372036854775808") == CalculationError::Overflow);
    REQUIRE(Error("9223372036854775807 + 1") == CalculationError::Overflow);
    REQUIRE(Error("4294967296 * 4294967296") == CalculationError::Overflow);
}
