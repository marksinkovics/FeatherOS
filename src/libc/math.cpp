#include "libc/math_functions.h"

#include <errno.h>
#include <math.h>

namespace Math = FeatherOS::LibC::Math;

namespace
{

// A finite argument whose result overflowed, or became zero from a nonzero exact value.
double RangeChecked(double argument, double result)
{
    if (!Math::IsNaN(argument) && !Math::IsInfinite(argument) &&
        (Math::IsInfinite(result) || result == 0))
    {
        errno = ERANGE;
    }
    return result;
}

double DomainChecked(double result)
{
    if (Math::IsNaN(result))
    {
        errno = EDOM;
    }
    return result;
}

// Only overflow is an error: sinh and cosh of a finite argument never underflow to 0.
double OverflowChecked(double argument, double result)
{
    if (Math::IsInfinite(result) && !Math::IsInfinite(argument))
    {
        errno = ERANGE;
    }
    return result;
}

// NaN arguments give NaN without a domain error.
bool AnyNaN(double x, double y = 0)
{
    return Math::IsNaN(x) || Math::IsNaN(y);
}

} // namespace

double fabs(double x)
{
    return Math::Abs(x);
}

double copysign(double magnitude, double sign)
{
    return Math::CopySign(magnitude, sign);
}

double floor(double x)
{
    return Math::Floor(x);
}

double ceil(double x)
{
    return Math::Ceil(x);
}

double trunc(double x)
{
    return Math::Trunc(x);
}

double round(double x)
{
    return Math::Round(x);
}

double fmod(double x, double y)
{
    return AnyNaN(x, y) ? x + y : DomainChecked(Math::Remainder(x, y));
}

double frexp(double x, int* exponent)
{
    return Math::SplitExponent(x, exponent);
}

double ldexp(double x, int exponent)
{
    return scalbn(x, exponent);
}

double scalbn(double x, int exponent)
{
    const double result = Math::ScaleB(x, exponent);
    return x == 0 ? result : RangeChecked(x, result);
}

double modf(double x, double* integer)
{
    return Math::SplitInteger(x, integer);
}

// A NaN loses to a number, as C requires.
double fmin(double x, double y)
{
    if (Math::IsNaN(x))
    {
        return y;
    }
    if (Math::IsNaN(y))
    {
        return x;
    }
    return x < y || (x == y && (Math::Bits(x) & Math::SignBit) != 0) ? x : y;
}

double fmax(double x, double y)
{
    if (Math::IsNaN(x))
    {
        return y;
    }
    if (Math::IsNaN(y))
    {
        return x;
    }
    return x > y || (x == y && (Math::Bits(x) & Math::SignBit) == 0) ? x : y;
}

double sqrt(double x)
{
    return AnyNaN(x) ? x : DomainChecked(Math::Sqrt(x));
}

double hypot(double x, double y)
{
    const double result = Math::Hypot(x, y);
    if (Math::IsInfinite(result) && !Math::IsInfinite(x) && !Math::IsInfinite(y))
    {
        errno = ERANGE;
    }
    return result;
}

double exp(double x)
{
    return RangeChecked(x, Math::Exp(x));
}

double exp2(double x)
{
    return RangeChecked(x, Math::Exp2(x));
}

// log of 0 is a pole error (ERANGE); of a negative number, a domain error.
double log(double x)
{
    if (x == 0)
    {
        errno = ERANGE;
    }
    return AnyNaN(x) ? x : DomainChecked(Math::Log(x));
}

double log2(double x)
{
    if (x == 0)
    {
        errno = ERANGE;
    }
    return AnyNaN(x) ? x : DomainChecked(Math::Log2(x));
}

double log10(double x)
{
    if (x == 0)
    {
        errno = ERANGE;
    }
    return AnyNaN(x) ? x : DomainChecked(Math::Log10(x));
}

double pow(double x, double y)
{
    const double result = Math::Pow(x, y);
    if (AnyNaN(x, y))
    {
        return result;
    }
    if (Math::IsNaN(result))
    {
        errno = EDOM;
    }
    else if (x == 0 && y < 0)
    {
        errno = ERANGE;
    }
    else if (!Math::IsInfinite(x) && !Math::IsInfinite(y) &&
             (Math::IsInfinite(result) || result == 0) && x != 0)
    {
        errno = ERANGE;
    }
    return result;
}

double sin(double x)
{
    return AnyNaN(x) ? x : DomainChecked(Math::Sin(x));
}

double cos(double x)
{
    return AnyNaN(x) ? x : DomainChecked(Math::Cos(x));
}

double tan(double x)
{
    return AnyNaN(x) ? x : DomainChecked(Math::Tan(x));
}

double asin(double x)
{
    return AnyNaN(x) ? x : DomainChecked(Math::ArcSin(x));
}

double acos(double x)
{
    return AnyNaN(x) ? x : DomainChecked(Math::ArcCos(x));
}

double atan(double x)
{
    return Math::ArcTan(x);
}

double atan2(double y, double x)
{
    return Math::ArcTan2(y, x);
}

double sinh(double x)
{
    return OverflowChecked(x, Math::Sinh(x));
}

double cosh(double x)
{
    return OverflowChecked(x, Math::Cosh(x));
}

double tanh(double x)
{
    return Math::Tanh(x);
}

float fabsf(float x)
{
    return static_cast<float>(::fabs(static_cast<double>(x)));
}

float copysignf(float magnitude, float sign)
{
    return static_cast<float>(
        ::copysign(static_cast<double>(magnitude), static_cast<double>(sign)));
}

float floorf(float x)
{
    return static_cast<float>(::floor(static_cast<double>(x)));
}

float ceilf(float x)
{
    return static_cast<float>(::ceil(static_cast<double>(x)));
}

float truncf(float x)
{
    return static_cast<float>(::trunc(static_cast<double>(x)));
}

float roundf(float x)
{
    return static_cast<float>(::round(static_cast<double>(x)));
}

float fmodf(float x, float y)
{
    return static_cast<float>(::fmod(static_cast<double>(x), static_cast<double>(y)));
}

float fminf(float x, float y)
{
    return static_cast<float>(::fmin(static_cast<double>(x), static_cast<double>(y)));
}

float fmaxf(float x, float y)
{
    return static_cast<float>(::fmax(static_cast<double>(x), static_cast<double>(y)));
}

float sqrtf(float x)
{
    return static_cast<float>(::sqrt(static_cast<double>(x)));
}

float hypotf(float x, float y)
{
    return static_cast<float>(::hypot(static_cast<double>(x), static_cast<double>(y)));
}

float expf(float x)
{
    return static_cast<float>(::exp(static_cast<double>(x)));
}

float exp2f(float x)
{
    return static_cast<float>(::exp2(static_cast<double>(x)));
}

float logf(float x)
{
    return static_cast<float>(::log(static_cast<double>(x)));
}

float log2f(float x)
{
    return static_cast<float>(::log2(static_cast<double>(x)));
}

float log10f(float x)
{
    return static_cast<float>(::log10(static_cast<double>(x)));
}

float powf(float x, float y)
{
    return static_cast<float>(::pow(static_cast<double>(x), static_cast<double>(y)));
}

float sinf(float x)
{
    return static_cast<float>(::sin(static_cast<double>(x)));
}

float cosf(float x)
{
    return static_cast<float>(::cos(static_cast<double>(x)));
}

float tanf(float x)
{
    return static_cast<float>(::tan(static_cast<double>(x)));
}

float asinf(float x)
{
    return static_cast<float>(::asin(static_cast<double>(x)));
}

float acosf(float x)
{
    return static_cast<float>(::acos(static_cast<double>(x)));
}

float atanf(float x)
{
    return static_cast<float>(::atan(static_cast<double>(x)));
}

float atan2f(float y, float x)
{
    return static_cast<float>(::atan2(static_cast<double>(y), static_cast<double>(x)));
}

float sinhf(float x)
{
    return static_cast<float>(::sinh(static_cast<double>(x)));
}

float coshf(float x)
{
    return static_cast<float>(::cosh(static_cast<double>(x)));
}

float tanhf(float x)
{
    return static_cast<float>(::tanh(static_cast<double>(x)));
}
