#include "math_functions.h"

#include <stdint.h>

namespace FeatherOS::LibC::Math
{

uint64_t Bits(double value)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double FromBits(uint64_t bits)
{
    double value;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

bool IsNaN(double value)
{
    return value != value;
}

bool IsInfinite(double value)
{
    return (Bits(value) & ~SignBit) == 0x7FF0000000000000ULL;
}

double Abs(double value)
{
    return FromBits(Bits(value) & ~SignBit);
}

double CopySign(double magnitude, double sign)
{
    return FromBits((Bits(magnitude) & ~SignBit) | (Bits(sign) & SignBit));
}

double Sqrt(double value)
{
    double result;
#if defined(__x86_64__)
    asm("sqrtsd %1, %0" : "=x"(result) : "x"(value));
#elif defined(__aarch64__)
    asm("fsqrt %d0, %d1" : "=w"(result) : "w"(value));
#else
#error "Unsupported architecture"
#endif
    return result;
}

double Trunc(double value)
{
    const uint64_t bits = Bits(value);
    const int exponent = static_cast<int>((bits >> 52) & 0x7FF) - 1023;
    if (exponent >= 52)
    {
        return value;
    }
    if (exponent < 0)
    {
        return FromBits(bits & SignBit);
    }
    return FromBits(bits & ~(FractionMask >> exponent));
}

double Floor(double value)
{
    const double truncated = Trunc(value);
    return value < 0 && truncated != value ? truncated - 1 : truncated;
}

double Ceil(double value)
{
    const double truncated = Trunc(value);
    return value > 0 && truncated != value ? truncated + 1 : truncated;
}

double Round(double value)
{
    if (!(Abs(value) < 0x1p52))
    {
        return value;
    }
    const double truncated = Trunc(value);
    return Abs(value - truncated) >= 0.5 ? truncated + CopySign(1.0, value) : truncated;
}

double ScaleB(double value, int exponent)
{
    if (exponent > 1023)
    {
        value *= 0x1p1023;
        exponent -= 1023;
        if (exponent > 1023)
        {
            value *= 0x1p1023;
            exponent -= 1023;
            exponent = exponent > 1023 ? 1023 : exponent;
        }
    }
    else if (exponent < -1022)
    {
        // Scaling by 2^-969 at a time keeps the intermediate results normal, so only the last
        // multiplication rounds.
        value *= 0x1p-1022 * 0x1p53;
        exponent += 1022 - 53;
        if (exponent < -1022)
        {
            value *= 0x1p-1022 * 0x1p53;
            exponent += 1022 - 53;
            exponent = exponent < -1022 ? -1022 : exponent;
        }
    }
    return value * FromBits(static_cast<uint64_t>(exponent + 1023) << 52);
}

double SplitExponent(double value, int* exponent)
{
    *exponent = 0;
    uint64_t bits = Bits(value);
    int biased = static_cast<int>((bits >> 52) & 0x7FF);
    if (biased == 0x7FF || (bits & ~SignBit) == 0)
    {
        return value;
    }
    if (biased == 0)
    {
        value *= 0x1p64;
        bits = Bits(value);
        biased = static_cast<int>((bits >> 52) & 0x7FF);
        *exponent = -64;
    }
    *exponent += biased - 1022;
    return FromBits((bits & ~(0x7FFULL << 52)) | (1022ULL << 52));
}

double SplitInteger(double value, double* integer)
{
    *integer = Trunc(value);
    if (IsInfinite(value))
    {
        return CopySign(0.0, value);
    }
    return CopySign(value - *integer, value);
}

double Remainder(double x, double y)
{
    const uint64_t x_bits = Bits(x);
    uint64_t magnitude_x = x_bits & ~SignBit;
    const uint64_t magnitude_y = Bits(y) & ~SignBit;
    if (IsNaN(x) || IsNaN(y) || magnitude_y == 0 || (magnitude_x >> 52) == 0x7FF)
    {
        return (x * y) / (x * y);
    }
    if (magnitude_x < magnitude_y)
    {
        return x;
    }
    // Normalized mantissas with the leading bit at 52, and their exponents.
    auto normalize = [](uint64_t bits, int* exponent) -> uint64_t
    {
        *exponent = static_cast<int>(bits >> 52);
        uint64_t mantissa = bits & FractionMask;
        if (*exponent == 0)
        {
            *exponent = 1;
            while ((mantissa & (1ULL << 52)) == 0)
            {
                mantissa <<= 1;
                --*exponent;
            }
            return mantissa;
        }
        return mantissa | (1ULL << 52);
    };
    int exponent_x;
    int exponent_y;
    uint64_t mantissa_x = normalize(magnitude_x, &exponent_x);
    const uint64_t mantissa_y = normalize(magnitude_y, &exponent_y);
    for (; exponent_x > exponent_y; --exponent_x)
    {
        if (mantissa_x >= mantissa_y)
        {
            mantissa_x -= mantissa_y;
        }
        if (mantissa_x == 0)
        {
            return FromBits(x_bits & SignBit);
        }
        mantissa_x <<= 1;
    }
    if (mantissa_x >= mantissa_y)
    {
        mantissa_x -= mantissa_y;
    }
    if (mantissa_x == 0)
    {
        return FromBits(x_bits & SignBit);
    }
    while ((mantissa_x & (1ULL << 52)) == 0)
    {
        mantissa_x <<= 1;
        --exponent_x;
    }
    if (exponent_x <= 0)
    {
        magnitude_x = mantissa_x >> (1 - exponent_x);
    }
    else
    {
        magnitude_x = (static_cast<uint64_t>(exponent_x) << 52) | (mantissa_x & FractionMask);
    }
    return FromBits(magnitude_x | (x_bits & SignBit));
}

int Nearest(double value)
{
    return static_cast<int>(value + CopySign(0.5, value));
}

double ExpKernel(double r)
{
    double sum = 1.0 / 6227020800.0;
    constexpr double factorials[] = {479001600.0, 39916800.0, 3628800.0, 362880.0, 40320.0, 5040.0,
                                     720.0,       120.0,      24.0,      6.0,      2.0,     1.0};
    for (const double factorial : factorials)
    {
        sum = sum * r + 1.0 / factorial;
    }
    return sum * r + 1.0;
}

double Exp(double value)
{
    if (IsNaN(value))
    {
        return value;
    }
    if (value > 0x1.62e42fefa39efp+9)
    {
        return Infinity;
    }
    if (value < -0x1.74910d52d3052p+9)
    {
        return 0.0;
    }
    const int k = Nearest(value * InverseLn2);
    const double r = (value - k * Ln2High) - k * Ln2Low;
    return ScaleB(ExpKernel(r), k);
}

double Exp2(double value)
{
    if (IsNaN(value))
    {
        return value;
    }
    if (value >= 1024)
    {
        return Infinity;
    }
    if (value < -1075)
    {
        return 0.0;
    }
    const int k = Nearest(value);
    return ScaleB(Exp(static_cast<double>(value - k) * Ln2), k);
}

void ExactSum(double a, double b, double* sum, double* error)
{
    *sum = a + b;
    const double b_part = *sum - a;
    *error = (a - (*sum - b_part)) + (b - b_part);
}

void ExactProduct(double a, double b, double* high, double* low)
{
    constexpr double Splitter = 0x1p27 + 1;
    const double a_split = a * Splitter;
    const double a_high = a_split - (a_split - a);
    const double a_low = a - a_high;
    const double b_split = b * Splitter;
    const double b_high = b_split - (b_split - b);
    const double b_low = b - b_high;
    *high = a * b;
    *low = ((a_high * b_high - *high) + a_high * b_low + a_low * b_high) + a_low * b_low;
}

void LogParts(double value, double* high, double* low, int* exponent, double* log_m)
{
    int k;
    double m = SplitExponent(value, &k);
    if (m < 0x1.6a09e667f3bcdp-1)
    {
        m *= 2;
        --k;
    }
    // m - 1 is exact here; m + 1 may not be, so its rounding error is kept.
    double denominator;
    double denominator_error;
    ExactSum(m, 1.0, &denominator, &denominator_error);
    const double f = (m - 1.0) / denominator;
    double product_high;
    double product_low;
    ExactProduct(f, denominator, &product_high, &product_low);
    const double residual = (((m - 1.0) - product_high) - product_low) - f * denominator_error;
    const double f_low = residual / denominator;

    const double s = f * f;
    double series = 1.0 / 23.0;
    for (int odd = 21; odd >= 3; odd -= 2)
    {
        series = series * s + 1.0 / odd;
    }
    const double twice_f = 2.0 * f;
    const double tail = 2.0 * f_low + twice_f * s * series;
    *log_m = twice_f + tail;
    *exponent = k;
    // k * Ln2High is exact: Ln2High has trailing zero bits to spare.
    double sum;
    double sum_error;
    ExactSum(k * Ln2High, twice_f, &sum, &sum_error);
    *high = sum;
    *low = sum_error + (tail + k * Ln2Low);
}

double Log(double value)
{
    if (IsNaN(value) || value < 0)
    {
        return __builtin_nan("");
    }
    if (value == 0)
    {
        return -Infinity;
    }
    if (IsInfinite(value))
    {
        return value;
    }
    double high;
    double low;
    int exponent;
    double log_m;
    LogParts(value, &high, &low, &exponent, &log_m);
    return high + low;
}

double Log2(double value)
{
    if (IsNaN(value) || value < 0 || value == 0 || IsInfinite(value))
    {
        return Log(value);
    }
    double high;
    double low;
    int exponent;
    double log_m;
    LogParts(value, &high, &low, &exponent, &log_m);
    return exponent + log_m * InverseLn2;
}

double Log10(double value)
{
    if (IsNaN(value) || value < 0 || value == 0 || IsInfinite(value))
    {
        return Log(value);
    }
    double high;
    double low;
    int exponent;
    double log_m;
    LogParts(value, &high, &low, &exponent, &log_m);
    // log10(2) in two parts, the first with trailing zero bits so exponent * high is exact.
    constexpr double Log10Of2High = 0x1.34413509f7800p-2;
    constexpr double Log10Of2Low = 0x1.fef311f12b358p-46;
    return exponent * Log10Of2High + (exponent * Log10Of2Low + log_m * InverseLn10);
}

bool IsInteger(double value)
{
    return Trunc(value) == value && !IsInfinite(value);
}

bool IsOddInteger(double value)
{
    return IsInteger(value) && Abs(value) < 0x1p53 && Remainder(value, 2.0) != 0;
}

double Pow(double x, double y)
{
    if (y == 0 || x == 1.0)
    {
        return 1.0;
    }
    if (IsNaN(x) || IsNaN(y))
    {
        return x + y;
    }
    const bool odd = IsOddInteger(y);
    if (x == 0)
    {
        if (y < 0)
        {
            return odd ? CopySign(Infinity, x) : Infinity;
        }
        return odd ? x : 0.0;
    }
    if (IsInfinite(y))
    {
        if (x == -1.0)
        {
            return 1.0;
        }
        return (Abs(x) > 1) == (y > 0) ? Infinity : 0.0;
    }
    if (IsInfinite(x))
    {
        const double magnitude = y > 0 ? Infinity : 0.0;
        return x < 0 && odd ? -magnitude : magnitude;
    }
    if (x < 0 && !IsInteger(y))
    {
        return __builtin_nan("");
    }
    const double sign = x < 0 && odd ? -1.0 : 1.0;
    const double base = Abs(x);
    double high;
    double low;
    int exponent;
    double log_m;
    LogParts(base, &high, &low, &exponent, &log_m);
    double product_high;
    double product_low;
    ExactProduct(y, high, &product_high, &product_low);
    product_low += y * low;
    const double exponent_sum = product_high + product_low;
    const double exponent_low = (product_high - exponent_sum) + product_low;
    if (exponent_sum > 0x1.62e42fefa39efp+9)
    {
        return sign * Infinity;
    }
    if (exponent_sum < -0x1.74910d52d3052p+9)
    {
        return sign * 0.0;
    }
    const int k = Nearest(exponent_sum * InverseLn2);
    const double r = ((exponent_sum - k * Ln2High) - k * Ln2Low) + exponent_low;
    return sign * ScaleB(ExpKernel(r), k);
}

double SinKernel(double r)
{
    // sin(r) rounds to r here, and returning r keeps the sign of -0.
    if (Abs(r) < 0x1p-27)
    {
        return r;
    }
    const double s = r * r;
    double sum = -1.0 / 121645100408832000.0;
    constexpr double factorials[] = {355687428096000.0, -1307674368000.0, 6227020800.0, -39916800.0,
                                     362880.0,          -5040.0,          120.0,        -6.0};
    for (const double factorial : factorials)
    {
        sum = sum * s + 1.0 / factorial;
    }
    return r + r * s * sum;
}

double CosKernel(double r)
{
    const double s = r * r;
    double sum = 1.0 / 2432902008176640000.0;
    constexpr double factorials[] = {
        -6402373705728000.0, 20922789888000.0, -87178291200.0, 479001600.0,
        -3628800.0,          40320.0,          -720.0,         24.0};
    for (const double factorial : factorials)
    {
        sum = sum * s + 1.0 / factorial;
    }
    return 1.0 - 0.5 * s + s * s * sum;
}

double ReduceQuarterTurns(double value, int* quadrant)
{
    constexpr double Part1 = 0x1.921fb54400000p+0;
    constexpr double Part2 = 0x1.0b4611a600000p-34;
    constexpr double Part3 = 0x1.3198a2e000000p-69;
    constexpr double Part3Tail = 0x1.b839a252049c1p-104;
    if (Abs(value) <= 0x1.921fb54442d18p-1)
    {
        *quadrant = 0;
        return value;
    }
    const double turns = Trunc(value * 0x1.45f306dc9c883p-1 + CopySign(0.5, value));
    *quadrant = static_cast<int>(static_cast<int64_t>(Remainder(turns, 4.0)) & 3);
    return (((value - turns * Part1) - turns * Part2) - turns * Part3) - turns * Part3Tail;
}

double Sin(double value)
{
    if (IsNaN(value) || IsInfinite(value))
    {
        return value - value;
    }
    int quadrant;
    const double r = ReduceQuarterTurns(value, &quadrant);
    switch (quadrant)
    {
    case 0:
        return SinKernel(r);
    case 1:
        return CosKernel(r);
    case 2:
        return -SinKernel(r);
    default:
        return -CosKernel(r);
    }
}

double Cos(double value)
{
    if (IsNaN(value) || IsInfinite(value))
    {
        return value - value;
    }
    int quadrant;
    const double r = ReduceQuarterTurns(value, &quadrant);
    switch (quadrant)
    {
    case 0:
        return CosKernel(r);
    case 1:
        return -SinKernel(r);
    case 2:
        return -CosKernel(r);
    default:
        return SinKernel(r);
    }
}

double Tan(double value)
{
    if (IsNaN(value) || IsInfinite(value))
    {
        return value - value;
    }
    int quadrant;
    const double r = ReduceQuarterTurns(value, &quadrant);
    const double sine = SinKernel(r);
    const double cosine = CosKernel(r);
    return (quadrant & 1) == 0 ? sine / cosine : -cosine / sine;
}

double ArcTanKernel(double t)
{
    constexpr double TanPiOver12 = 0x1.126145e9ecd56p-2;
    constexpr double Sqrt3 = 0x1.bb67ae8584caap+0;
    constexpr double PiOver6 = 0x1.0c152382d7366p-1;
    constexpr double PiOver6Low = -0x1.ee6913347c2a6p-55;
    double offset = 0;
    double offset_low = 0;
    double u = t;
    if (t > TanPiOver12)
    {
        u = (t * Sqrt3 - 1.0) / (Sqrt3 + t);
        offset = PiOver6;
        offset_low = PiOver6Low;
    }
    // u + u s (-1/3 + s/5 - s^2/7 + ... - s^14/31), summed from the smallest term.
    const double s = u * u;
    double series = -1.0 / 31.0;
    for (int term = 14; term >= 1; --term)
    {
        series = series * s + (term % 2 == 0 ? 1.0 : -1.0) / (2 * term + 1);
    }
    return offset + (u + (u * s * series + offset_low));
}

double ArcTan(double value)
{
    if (IsNaN(value))
    {
        return value;
    }
    const double magnitude = Abs(value);
    double result;
    if (magnitude > 1)
    {
        result = (HalfPi - ArcTanKernel(1.0 / magnitude)) + HalfPiLow;
    }
    else
    {
        result = ArcTanKernel(magnitude);
    }
    return CopySign(result, value);
}

double ArcTan2(double y, double x)
{
    if (IsNaN(x) || IsNaN(y))
    {
        return x + y;
    }
    const bool negative_x = (Bits(x) & SignBit) != 0;
    if (y == 0)
    {
        return negative_x ? CopySign(Pi, y) : y;
    }
    if (x == 0)
    {
        return CopySign(HalfPi, y);
    }
    if (IsInfinite(x))
    {
        if (IsInfinite(y))
        {
            return CopySign(negative_x ? 3 * Pi / 4 : Pi / 4, y);
        }
        return negative_x ? CopySign(Pi, y) : CopySign(0.0, y);
    }
    if (IsInfinite(y))
    {
        return CopySign(HalfPi, y);
    }
    const double angle = ArcTan(Abs(y / x));
    return CopySign(negative_x ? (Pi - angle) + PiLow : angle, y);
}

double ArcSin(double value)
{
    if (!(Abs(value) <= 1))
    {
        return __builtin_nan("");
    }
    return ArcTan2(value, Sqrt((1.0 - value) * (1.0 + value)));
}

double ArcCos(double value)
{
    if (!(Abs(value) <= 1))
    {
        return __builtin_nan("");
    }
    return ArcTan2(Sqrt((1.0 - value) * (1.0 + value)), value);
}

double SinhSeries(double value)
{
    const double s = value * value;
    double sum = 1.0 / 355687428096000.0;
    constexpr double factorials[] = {1307674368000.0, 6227020800.0, 39916800.0, 362880.0,
                                     5040.0,          120.0,        6.0};
    for (const double factorial : factorials)
    {
        sum = sum * s + 1.0 / factorial;
    }
    return value + value * s * sum;
}

double HalfExp(double magnitude)
{
    if (magnitude < 709)
    {
        return 0.5 * Exp(magnitude);
    }
    const double half = Exp(0.5 * magnitude);
    return (0.5 * half) * half;
}

double Sinh(double value)
{
    if (IsNaN(value) || IsInfinite(value))
    {
        return value;
    }
    const double magnitude = Abs(value);
    if (magnitude < 0.5)
    {
        return SinhSeries(value);
    }
    if (magnitude > 22)
    {
        return CopySign(HalfExp(magnitude), value);
    }
    const double e = Exp(magnitude);
    return CopySign(0.5 * (e - 1.0 / e), value);
}

double Cosh(double value)
{
    if (IsNaN(value))
    {
        return value;
    }
    const double magnitude = Abs(value);
    if (magnitude > 22)
    {
        return HalfExp(magnitude);
    }
    const double e = Exp(magnitude);
    return 0.5 * (e + 1.0 / e);
}

double Tanh(double value)
{
    if (IsNaN(value))
    {
        return value;
    }
    const double magnitude = Abs(value);
    if (magnitude > 22)
    {
        return CopySign(1.0, value);
    }
    if (magnitude < 0.5)
    {
        const double sinh = SinhSeries(magnitude);
        return CopySign(sinh / Sqrt(1.0 + sinh * sinh), value);
    }
    const double e = Exp(2.0 * magnitude);
    return CopySign(1.0 - 2.0 / (e + 1.0), value);
}

double Hypot(double x, double y)
{
    if (IsInfinite(x) || IsInfinite(y))
    {
        return Infinity;
    }
    if (IsNaN(x) || IsNaN(y))
    {
        return x + y;
    }
    double larger = Abs(x);
    double smaller = Abs(y);
    if (smaller > larger)
    {
        const double swap = larger;
        larger = smaller;
        smaller = swap;
    }
    if (larger == 0)
    {
        return 0.0;
    }
    const double ratio = smaller / larger;
    return larger * Sqrt(1.0 + ratio * ratio);
}

} // namespace FeatherOS::LibC::Math
