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

// Exact: fabs, copysign, floor, ceil, trunc, round, fmod, frexp, ldexp, modf, sqrt, fmin, fmax.
// The rest are within a few units in the last place; sin, cos and tan lose precision for
// |x| above about 10^6. There are no long double functions yet.
double fabs(double x);
double copysign(double magnitude, double sign);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double frexp(double x, int* exponent);
double ldexp(double x, int exponent);
double scalbn(double x, int exponent);
double modf(double x, double* integer);
double fmin(double x, double y);
double fmax(double x, double y);
double sqrt(double x);
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
float fmodf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);
float sqrtf(float x);
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

__END_DECLS

#endif
