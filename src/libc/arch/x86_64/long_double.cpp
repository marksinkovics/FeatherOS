#include <bit>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// long double is the x87 80-bit format, whose instructions do most of the work. Programs start
// with the x87 unit at 64-bit precision, rounding to nearest, every exception masked.
namespace
{

constexpr uint16_t RoundingControlMask = 0x0C00;
constexpr uint16_t RoundDown = 0x0400;
constexpr uint16_t RoundUp = 0x0800;
constexpr uint16_t RoundTowardZero = 0x0C00;

constexpr long double Ln2 = 0.693147180559945309417232121458176568L;
constexpr long double Log2E = 1.442695040888963407359924681001892137L;
constexpr long double Log10Of2 = 0.301029995663981195213738894724493027L;
// ln 2 split so that n * Ln2High is exact for every |n| below 2^16: Ln2High keeps 48 bits.
constexpr long double Ln2High = 0x1.62e42fefa39ep-1L;
constexpr long double Ln2Low = Ln2 - Ln2High;

bool IsNaN(long double value)
{
    return __builtin_isnan(value);
}

bool IsInfinite(long double value)
{
    return __builtin_isinf(value);
}

long double RoundWith(long double value, uint16_t rounding)
{
    uint16_t saved = 0;
    asm volatile("fnstcw %0" : "=m"(saved));
    const uint16_t changed = static_cast<uint16_t>((saved & ~RoundingControlMask) | rounding);
    asm volatile("fldcw %0" : : "m"(changed));
    asm volatile("frndint" : "+t"(value));
    asm volatile("fldcw %0" : : "m"(saved));
    return value;
}

// fprem truncates the quotient (fmod) and fprem1 rounds it to nearest (remainder); both are exact
// and reduce by at most 2^63 per step, setting C2 until they are done.
long double PartialRemainder(long double x, long double y, bool nearest)
{
    long double result = 0;
    if (nearest)
    {
        asm("1:\n\tfprem1\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b"
            : "=t"(result)
            : "0"(x), "u"(y)
            : "ax", "cc");
    }
    else
    {
        asm("1:\n\tfprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b"
            : "=t"(result)
            : "0"(x), "u"(y)
            : "ax", "cc");
    }
    return result;
}

long double Remainder(long double x, long double y, bool nearest)
{
    if (IsNaN(x) || IsNaN(y))
    {
        return x + y;
    }
    const long double result = PartialRemainder(x, y, nearest);
    if (IsNaN(result))
    {
        errno = EDOM;
    }
    return result;
}

// value * 2^exponent for an integral exponent.
long double Scale(long double value, long double exponent)
{
    long double result = 0;
    asm("fscale" : "=t"(result) : "0"(value), "u"(exponent));
    return result;
}

// 2^fraction - 1 for |fraction| <= 1.
long double PowerOfTwoMinusOne(long double fraction)
{
    asm("f2xm1" : "+t"(fraction));
    return fraction;
}

// An overflow or an underflow to zero from a finite argument.
long double RangeChecked(long double argument, long double result)
{
    if (!IsInfinite(argument) && (IsInfinite(result) || result == 0))
    {
        errno = ERANGE;
    }
    return result;
}

long double ExpOfLog2(long double whole, long double fraction)
{
    return Scale(PowerOfTwoMinusOne(fraction) + 1, whole);
}

// Exponents far beyond the format's range are clamped, so fscale still overflows or underflows.
long double Exp2(long double value)
{
    if (IsNaN(value))
    {
        return value;
    }
    if (IsInfinite(value))
    {
        return value > 0 ? value : 0;
    }
    constexpr long double Limit = 20000;
    if (value > Limit || value < -Limit)
    {
        return RangeChecked(value, Scale(1, value > 0 ? Limit : -Limit));
    }
    const long double whole = rintl(value);
    return RangeChecked(value, ExpOfLog2(whole, value - whole));
}

// value * log2(argument), which fyl2x computes and checks: NaN below zero, -inf at zero.
long double Logarithm(long double argument, long double factor)
{
    long double result = 0;
    asm("fyl2x" : "=t"(result) : "0"(argument), "u"(factor) : "st(1)");
    if (IsNaN(argument))
    {
        return result;
    }
    if (argument < 0)
    {
        errno = EDOM;
    }
    else if (argument == 0)
    {
        errno = ERANGE;
    }
    return result;
}

// Taylor series on |r| <= pi/4, where the terms through r^23 reach the last place. Factorials
// up to 23! are exact in this format, so each coefficient is rounded once.
constexpr long double OddFactorials[] = {25852016738884976640000.0L,
                                         51090942171709440000.0L,
                                         121645100408832000.0L,
                                         355687428096000.0L,
                                         1307674368000.0L,
                                         6227020800.0L,
                                         39916800.0L,
                                         362880.0L,
                                         5040.0L,
                                         120.0L,
                                         6.0L};
constexpr long double EvenFactorials[] = {1124000727777607680000.0L,
                                          2432902008176640000.0L,
                                          6402373705728000.0L,
                                          20922789888000.0L,
                                          87178291200.0L,
                                          479001600.0L,
                                          3628800.0L,
                                          40320.0L,
                                          720.0L,
                                          24.0L,
                                          2.0L};

// The sum of (-1)^k square^k / factorial over the factorials given, highest first, which
// alternate in sign and end with k = 1.
template <size_t Count>
long double AlternatingSeries(long double square, const long double (&factorials)[Count])
{
    long double sum = 0;
    for (size_t index = 0; index < Count; ++index)
    {
        const long double term = 1 / factorials[index];
        sum = sum * square + ((Count - 1 - index) % 2 == 0 ? -term : term);
    }
    return sum * square;
}

long double SinKernel(long double r)
{
    return r + r * AlternatingSeries(r * r, OddFactorials);
}

long double CosKernel(long double r)
{
    return 1 + AlternatingSeries(r * r, EvenFactorials);
}

// pi/2 in three parts, the first two with 33 significant bits, so that n times either is exact
// for |n| below 2^31 and r = x - n pi/2 keeps its low bits (Cody and Waite).
constexpr long double TwoOverPi = 0x1.45f306dc9c882a54p-1L;
constexpr long double HalfPiHigh = 0x1.921fb544p0L;
constexpr long double HalfPiMiddle = 0x1.0b4611a6p-34L;
constexpr long double HalfPiLow = 0x1.3198a2e03707344ap-69L;
constexpr long double TwoPi = 0x1.921fb54442d1846ap2L;

// The x87 unit's fsin and fcos are not used: QEMU computes them in double precision. Beyond 2^30
// the argument is first reduced modulo the rounded 2pi, so precision drops there.
long double Trigonometric(long double value, bool cosine)
{
    if (IsNaN(value) || value == 0)
    {
        return cosine ? 1 + value : value;
    }
    if (IsInfinite(value))
    {
        errno = EDOM;
        return value - value;
    }
    if (__builtin_fabsl(value) >= 0x1p30L)
    {
        value = PartialRemainder(value, TwoPi, true);
    }
    const long double turns = rintl(value * TwoOverPi);
    const long double reduced =
        ((value - turns * HalfPiHigh) - turns * HalfPiMiddle) - turns * HalfPiLow;
    const int quadrant = static_cast<int>(static_cast<int64_t>(turns) & 3) + (cosine ? 1 : 0);
    switch (quadrant & 3)
    {
    case 0:
        return SinKernel(reduced);
    case 1:
        return CosKernel(reduced);
    case 2:
        return -SinKernel(reduced);
    default:
        return -CosKernel(reduced);
    }
}

// A 256-bit unsigned integer, least significant word first, for an exact fmal.
struct Wide256
{
    uint64_t words[4];
};

constexpr int WideBits = 256;
constexpr int WordBits = 64;

bool BitAt(const Wide256& value, int bit)
{
    return ((value.words[bit / WordBits] >> (bit % WordBits)) & 1) != 0;
}

int HighestBit(const Wide256& value)
{
    for (int word = 3; word >= 0; --word)
    {
        if (value.words[word] != 0)
        {
            return word * WordBits + std::bit_width(value.words[word]) - 1;
        }
    }
    return -1;
}

Wide256 ShiftLeft(const Wide256& value, int shift)
{
    Wide256 result{};
    for (int bit = HighestBit(value); bit >= 0; --bit)
    {
        if (BitAt(value, bit) && bit + shift < WideBits)
        {
            result.words[(bit + shift) / WordBits] |= 1ULL << ((bit + shift) % WordBits);
        }
    }
    return result;
}

// Bits shifted out are kept as one sticky bit at the bottom, enough to round correctly.
Wide256 ShiftRightSticky(const Wide256& value, int shift)
{
    Wide256 result{};
    bool sticky = false;
    for (int bit = HighestBit(value); bit >= 0; --bit)
    {
        if (!BitAt(value, bit))
        {
            continue;
        }
        if (bit >= shift)
        {
            result.words[(bit - shift) / WordBits] |= 1ULL << ((bit - shift) % WordBits);
        }
        else
        {
            sticky = true;
        }
    }
    if (sticky)
    {
        result.words[0] |= 1;
    }
    return result;
}

bool AtLeast(const Wide256& value, const Wide256& other)
{
    for (int word = 3; word >= 0; --word)
    {
        if (value.words[word] != other.words[word])
        {
            return value.words[word] > other.words[word];
        }
    }
    return true;
}

Wide256 Add(const Wide256& value, const Wide256& other)
{
    Wide256 result{};
    uint64_t carry = 0;
    for (int word = 0; word < 4; ++word)
    {
        const uint64_t sum = value.words[word] + other.words[word];
        const uint64_t carry_out = sum < value.words[word] ? 1 : 0;
        result.words[word] = sum + carry;
        carry = carry_out | (result.words[word] < sum ? 1 : 0);
    }
    return result;
}

Wide256 Subtract(const Wide256& value, const Wide256& other)
{
    Wide256 result{};
    uint64_t borrow = 0;
    for (int word = 0; word < 4; ++word)
    {
        const uint64_t difference = value.words[word] - other.words[word];
        const uint64_t borrow_out = value.words[word] < other.words[word] ? 1 : 0;
        result.words[word] = difference - borrow;
        borrow = borrow_out | (difference < borrow ? 1 : 0);
    }
    return result;
}

// value = mantissa * 2^exponent, the mantissa's top bit set unless the value is zero.
struct Decoded
{
    bool negative;
    uint64_t mantissa;
    int exponent;
};

// The format: a 64-bit significand with an explicit integer bit, then the sign and a 15-bit
// exponent biased by 16383. The significand's lowest bit is worth 2^(biased - 16383 - 63).
constexpr int ExponentBias = 16383;
constexpr int MantissaBits = 64;
constexpr int MaximumBiased = 0x7FFF;
constexpr int LowestPlace = 1 - ExponentBias - (MantissaBits - 1);

Decoded Decode(long double value)
{
    uint64_t mantissa = 0;
    uint16_t sign_exponent = 0;
    memcpy(&mantissa, &value, sizeof(mantissa));
    memcpy(&sign_exponent, reinterpret_cast<const char*>(&value) + sizeof(mantissa),
           sizeof(sign_exponent));
    const int biased = sign_exponent & MaximumBiased;
    Decoded decoded{.negative = (sign_exponent >> 15) != 0,
                    .mantissa = mantissa,
                    .exponent = (biased == 0 ? 1 : biased) - ExponentBias - (MantissaBits - 1)};
    if (mantissa != 0)
    {
        const int shift = std::countl_zero(mantissa);
        decoded.mantissa <<= shift;
        decoded.exponent -= shift;
    }
    return decoded;
}

long double Encode(bool negative, uint64_t mantissa, int biased)
{
    long double value = 0;
    const auto sign_exponent = static_cast<uint16_t>((negative ? 0x8000 : 0) | biased);
    memcpy(&value, &mantissa, sizeof(mantissa));
    memcpy(reinterpret_cast<char*>(&value) + sizeof(mantissa), &sign_exponent,
           sizeof(sign_exponent));
    return value;
}

// Rounds sum * 2^exponent to nearest, ties to even, as a normal or subnormal long double. The
// lowest kept bit is worth 2^place: 64 bits are kept, fewer for a subnormal.
long double RoundToFormat(bool negative, const Wide256& sum, int exponent)
{
    const int top = HighestBit(sum);
    int place = exponent + top - (MantissaBits - 1);
    place = place < LowestPlace ? LowestPlace : place;
    const int dropped = place - exponent;
    uint64_t kept = 0;
    bool half = false;
    bool rest = false;
    for (int bit = 0; bit <= top; ++bit)
    {
        if (!BitAt(sum, bit))
        {
            continue;
        }
        if (bit >= dropped)
        {
            kept |= 1ULL << (bit - dropped);
        }
        else if (bit == dropped - 1)
        {
            half = true;
        }
        else
        {
            rest = true;
        }
    }
    if (half && (rest || (kept & 1) != 0))
    {
        ++kept;
        if (kept == 0)
        {
            kept = 1ULL << (MantissaBits - 1);
            ++place;
        }
    }
    if ((kept >> (MantissaBits - 1)) == 0)
    {
        return Encode(negative, kept, 0);
    }
    const int biased = place - LowestPlace + 1;
    return biased >= MaximumBiased ? Encode(negative, 1ULL << (MantissaBits - 1), MaximumBiased)
                                   : Encode(negative, kept, biased);
}

long double Fused(long double x, long double y, long double z)
{
    const auto finite = [](long double value) { return !IsNaN(value) && !IsInfinite(value); };
    if (!finite(x) || !finite(y))
    {
        return x * y + z;
    }
    // x * y alone could overflow to the infinity of the other sign.
    if (!finite(z))
    {
        return z + z;
    }
    if (x == 0 || y == 0)
    {
        return x * y + z;
    }
    // Adding a zero to the rounded product would lose the sign of a product that underflows.
    if (z == 0)
    {
        return x * y;
    }
    const Decoded a = Decode(x);
    const Decoded b = Decode(y);
    const Decoded c = Decode(z);

    // Both operands are moved to the top of 256 bits, leaving one bit for a carry. The product has
    // 128 significant bits and the addend 64, so an alignment shift drops bits only when their
    // tops lie more than 126 places apart, and then a subtraction cancels at most one bit.
    constexpr int Top = WideBits - 2;
    const unsigned __int128 product = static_cast<unsigned __int128>(a.mantissa) * b.mantissa;
    Wide256 left{.words = {static_cast<uint64_t>(product), static_cast<uint64_t>(product >> 64)}};
    const int left_shift = Top - HighestBit(left);
    left = ShiftLeft(left, left_shift);
    const int left_exponent = a.exponent + b.exponent - left_shift;
    Wide256 right{.words = {c.mantissa}};
    const int right_shift = Top - HighestBit(right);
    right = ShiftLeft(right, right_shift);
    const int right_exponent = c.exponent - right_shift;

    int exponent = left_exponent;
    if (left_exponent >= right_exponent)
    {
        right = ShiftRightSticky(right, left_exponent - right_exponent);
    }
    else
    {
        left = ShiftRightSticky(left, right_exponent - left_exponent);
        exponent = right_exponent;
    }

    const bool product_negative = a.negative != b.negative;
    bool negative = product_negative;
    Wide256 sum{};
    if (product_negative == c.negative)
    {
        sum = Add(left, right);
    }
    else if (AtLeast(left, right))
    {
        sum = Subtract(left, right);
    }
    else
    {
        sum = Subtract(right, left);
        negative = c.negative;
    }
    // An exact zero from opposite signs is +0 when rounding to nearest.
    if (HighestBit(sum) < 0)
    {
        return 0;
    }
    return RoundToFormat(negative, sum, exponent);
}

} // namespace

long double fabsl(long double x)
{
    return __builtin_fabsl(x);
}

long double copysignl(long double magnitude, long double sign)
{
    return __builtin_copysignl(magnitude, sign);
}

long double floorl(long double x)
{
    return RoundWith(x, RoundDown);
}

long double ceill(long double x)
{
    return RoundWith(x, RoundUp);
}

long double truncl(long double x)
{
    return RoundWith(x, RoundTowardZero);
}

long double roundl(long double x)
{
    const long double truncated = truncl(x);
    return __builtin_fabsl(x - truncated) >= 0.5L ? truncated + __builtin_copysignl(1.0L, x)
                                                  : truncated;
}

long double rintl(long double x)
{
    asm("frndint" : "+t"(x));
    return x;
}

long double nearbyintl(long double x)
{
    return rintl(x);
}

long double fmodl(long double x, long double y)
{
    return Remainder(x, y, false);
}

long double remainderl(long double x, long double y)
{
    return Remainder(x, y, true);
}

long double sqrtl(long double x)
{
    if (x < 0)
    {
        errno = EDOM;
    }
    asm("fsqrt" : "+t"(x));
    return x;
}

long double fmal(long double x, long double y, long double z)
{
    return Fused(x, y, z);
}

long double exp2l(long double x)
{
    return Exp2(x);
}

// e^x = 2^n * e^r with r = x - n ln 2 and |r| <= ln 2 / 2; computing r in two parts keeps it
// exact, where x * log2(e) would lose the low bits of large arguments.
long double expl(long double x)
{
    if (IsNaN(x) || IsInfinite(x) || __builtin_fabsl(x) > 15000)
    {
        return Exp2(x * Log2E);
    }
    const long double whole = rintl(x * Log2E);
    const long double reduced = (x - whole * Ln2High) - whole * Ln2Low;
    return RangeChecked(x, ExpOfLog2(whole, reduced * Log2E));
}

long double logl(long double x)
{
    return Logarithm(x, Ln2);
}

long double log2l(long double x)
{
    return Logarithm(x, 1);
}

long double log10l(long double x)
{
    return Logarithm(x, Log10Of2);
}

long double sinl(long double x)
{
    return Trigonometric(x, false);
}

long double cosl(long double x)
{
    return Trigonometric(x, true);
}
