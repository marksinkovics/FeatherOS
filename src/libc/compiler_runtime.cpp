#include "libc/wide_division.h"

#include <stdint.h>

// GCC calls libgcc for operations the target has no instruction for, and programs do not link
// libgcc. At -O2, AArch64 shifts unsigned __int128 values right through __lshrti3. The shift is
// done on 64-bit halves, since a 128-bit shift here would call this function again.
extern "C" unsigned __int128 __lshrti3(unsigned __int128 value, int shift)
{
    constexpr int HalfBits = 64;
    auto low = static_cast<uint64_t>(value);
    auto high = static_cast<uint64_t>(value >> HalfBits);
    if (shift >= HalfBits)
    {
        low = high >> (shift - HalfBits);
        high = 0;
    }
    else if (shift > 0)
    {
        low = (low >> shift) | (high << (HalfBits - shift));
        high >>= shift;
    }
    return static_cast<unsigned __int128>(high) << HalfBits | low;
}

namespace
{

using FeatherOS::LibC::Wide;

constexpr int HalfBits = 64;

Wide Split(unsigned __int128 value)
{
    return {.high = static_cast<uint64_t>(value >> HalfBits), .low = static_cast<uint64_t>(value)};
}

unsigned __int128 Join(Wide value)
{
    return static_cast<unsigned __int128>(value.high) << HalfBits | value.low;
}

// Division by zero traps, as the hardware's divide instructions do on x86-64.
Wide Divide(unsigned __int128 dividend, unsigned __int128 divisor, Wide* remainder)
{
    if (divisor == 0)
    {
        __builtin_trap();
    }
    return FeatherOS::LibC::DivideWide(Split(dividend), Split(divisor), remainder);
}

unsigned __int128 Magnitude(__int128 value)
{
    const auto bits = static_cast<unsigned __int128>(value);
    return value < 0 ? 0 - bits : bits;
}

} // namespace

// Swift's Int128 and UInt128 and GCC's __int128 divide through these libgcc functions.
extern "C" unsigned __int128 __udivti3(unsigned __int128 dividend, unsigned __int128 divisor)
{
    Wide remainder{};
    return Join(Divide(dividend, divisor, &remainder));
}

extern "C" unsigned __int128 __umodti3(unsigned __int128 dividend, unsigned __int128 divisor)
{
    Wide remainder{};
    Divide(dividend, divisor, &remainder);
    return Join(remainder);
}

// The quotient truncates toward zero and the remainder takes the dividend's sign, as in C.
extern "C" __int128 __divti3(__int128 dividend, __int128 divisor)
{
    Wide remainder{};
    const unsigned __int128 quotient =
        Join(Divide(Magnitude(dividend), Magnitude(divisor), &remainder));
    return static_cast<__int128>((dividend < 0) != (divisor < 0) ? 0 - quotient : quotient);
}

extern "C" __int128 __modti3(__int128 dividend, __int128 divisor)
{
    Wide remainder{};
    Divide(Magnitude(dividend), Magnitude(divisor), &remainder);
    const unsigned __int128 magnitude = Join(remainder);
    return static_cast<__int128>(dividend < 0 ? 0 - magnitude : magnitude);
}
