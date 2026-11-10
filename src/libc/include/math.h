#ifndef FEATHEROS_LIBC_MATH_H
#define FEATHEROS_LIBC_MATH_H

#include <sys/cdefs.h>

#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
// Errors set errno: EDOM outside a function's domain, ERANGE on overflow, underflow, or a pole.
#define MATH_ERRNO 1
#define math_errhandling MATH_ERRNO

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)

#define M_E 2.71828182845904523536
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_SQRT2 1.41421356237309504880

__BEGIN_DECLS

// Exact: fabs, copysign, floor, ceil, trunc, round, rint, nearbyint, fmod, remainder, frexp,
// ldexp, modf, sqrt, fmin, fmax, fma.
// The rest are within a few units in the last place; sin, cos and tan lose precision for
// |x| above about 10^6.
double fabs(double x);
double copysign(double magnitude, double sign);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double frexp(double x, int* exponent);
double ldexp(double x, int exponent);
double scalbn(double x, int exponent);
double modf(double x, double* integer);
double fmin(double x, double y);
double fmax(double x, double y);
double sqrt(double x);
double fma(double x, double y, double z);
double hypot(double x, double y);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

float fabsf(float x);
float copysignf(float magnitude, float sign);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float rintf(float x);
float nearbyintf(float x);
float fmodf(float x, float y);
float remainderf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);
float sqrtf(float x);
float fmaf(float x, float y, float z);
float hypotf(float x, float y);
float expf(float x);
float exp2f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);

#if defined(__x86_64__)
// The x87 format. Exact: fabsl copysignl floorl ceill truncl roundl rintl nearbyintl fmodl
// remainderl sqrtl fmal. The rest are within a few units in the last place: expl, exp2l and the
// logarithms through the x87 unit's f2xm1 and fyl2x, sinl and cosl from series, which lose
// precision for |x| beyond 2^30. AArch64 has none of these.
long double fabsl(long double x);
long double copysignl(long double magnitude, long double sign);
long double floorl(long double x);
long double ceill(long double x);
long double truncl(long double x);
long double roundl(long double x);
long double rintl(long double x);
long double nearbyintl(long double x);
long double fmodl(long double x, long double y);
long double remainderl(long double x, long double y);
long double sqrtl(long double x);
long double fmal(long double x, long double y, long double z);
long double expl(long double x);
long double exp2l(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double sinl(long double x);
long double cosl(long double x);
#endif

__END_DECLS

#endif
