#ifndef FEATHEROS_LIBC_MATH_FUNCTIONS_H
#define FEATHEROS_LIBC_MATH_FUNCTIONS_H

#include <stdint.h>

// The double-precision math behind math.h, without errno (math.cpp sets it). Each function
// reduces its argument to a small interval and evaluates a truncated Taylor series there; the
// results are within a few units in the last place of the exact value, not correctly rounded.
// sin, cos and tan reduce by pi/2 in three exact parts, which is accurate for |x| below about
// 10^6 and loses precision beyond.
namespace FeatherOS::LibC::Math
{

uint64_t Bits(double value);
double FromBits(uint64_t bits);

constexpr uint64_t SignBit = 1ULL << 63;
constexpr uint64_t FractionMask = (1ULL << 52) - 1;
constexpr double Infinity = __builtin_inf();
constexpr double HalfPi = 0x1.921fb54442d18p+0;
constexpr double HalfPiLow = 0x1.1a62633145c07p-54;
constexpr double Pi = 0x1.921fb54442d18p+1;
constexpr double PiLow = 0x1.1a62633145c07p-53;
constexpr double Ln2 = 0x1.62e42fefa39efp-1;
// ln 2 split so that k * Ln2High is exact for any binary exponent k.
constexpr double Ln2High = 0x1.62e42fee00000p-1;
constexpr double Ln2Low = 0x1.a39ef35793c76p-33;
constexpr double InverseLn2 = 0x1.71547652b82fep+0;
constexpr double InverseLn10 = 0x1.bcb7b1526e50ep-2;

bool IsNaN(double value);
bool IsInfinite(double value);
double Abs(double value);
double CopySign(double magnitude, double sign);
double Sqrt(double value);
double Trunc(double value);
double Floor(double value);
double Ceil(double value);

// Halfway cases go away from zero.
double Round(double value);

// value * 2^exponent with one rounding, even when the result is subnormal.
double ScaleB(double value, int exponent);

// value = fraction * 2^exponent with |fraction| in [0.5, 1).
double SplitExponent(double value, int* exponent);

double SplitInteger(double value, double* integer);

// The exact remainder of x / y, computed on the integer mantissas.
double Remainder(double x, double y);

// Rounds to the nearest integer for argument reduction; |value| must fit in an int.
int Nearest(double value);

// e^r for |r| <= ln(2) / 2, where the series' 15th term is below 2^-62.
double ExpKernel(double r);

double Exp(double value);

// 2^value: the integer part only scales, so its rounding never grows with |value|.
double Exp2(double value);

// a + b exactly as sum + error (Knuth's two-sum).
void ExactSum(double a, double b, double* sum, double* error);

// a * b exactly as high + low (Dekker's product, splitting each factor into 26-bit halves).
void ExactProduct(double a, double b, double* high, double* low);

// ln(x) for finite x > 0 as high + low, which together carry more than double precision:
// x = m * 2^k with m in [sqrt(1/2), sqrt(2)), and ln(m) = 2 atanh(f) with f = (m - 1) / (m + 1).
// f itself is kept in two parts, since pow multiplies any error in it by y.
void LogParts(double value, double* high, double* low, int* exponent, double* log_m);

double Log(double value);
double Log2(double value);
double Log10(double value);
bool IsInteger(double value);
bool IsOddInteger(double value);

// x^y with C's special cases. The general case is e^(y ln x) with y ln x kept in two parts,
// since rounding it to one double would cost |y ln x| units in the last place of the result.
double Pow(double x, double y);

// sin(r) and cos(r) for |r| <= pi/4, where the series' next terms are below 2^-60.
double SinKernel(double r);

double CosKernel(double r);

// r = value - quadrant * pi/2, with pi/2 in three 33-bit parts so each product is exact.
double ReduceQuarterTurns(double value, int* quadrant);

double Sin(double value);
double Cos(double value);
double Tan(double value);

// atan for 0 <= t <= 1: above tan(pi/12), atan(t) = pi/6 + atan((t sqrt3 - 1) / (sqrt3 + t)),
// which leaves |u| <= tan(pi/12) for the series u - u^3/3 + u^5/5 - ...
double ArcTanKernel(double t);

double ArcTan(double value);
double ArcTan2(double y, double x);
double ArcSin(double value);
double ArcCos(double value);

// sinh(x) for |x| < 0.5, where e^x - e^-x would cancel most digits.
double SinhSeries(double value);

// e^|x| / 2 without overflowing early: near the limit, (e^(|x|/2) / 2) * e^(|x|/2).
double HalfExp(double magnitude);

double Sinh(double value);
double Cosh(double value);
double Tanh(double value);
double Hypot(double x, double y);

} // namespace FeatherOS::LibC::Math

#endif
