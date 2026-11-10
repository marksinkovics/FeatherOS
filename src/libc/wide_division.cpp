#include "libc/wide_division.h"

namespace FeatherOS::LibC
{

namespace
{

bool AtLeast(Wide value, Wide other)
{
    return value.high != other.high ? value.high > other.high : value.low >= other.low;
}

Wide Subtract(Wide value, Wide other)
{
    const uint64_t borrow = value.low < other.low ? 1 : 0;
    return {.high = value.high - other.high - borrow, .low = value.low - other.low};
}

} // namespace

// Long division a bit at a time, from the dividend's highest set bit. The running remainder stays
// below the divisor, so doubling it overflows 128 bits only when it is then larger than the
// divisor, and the wrapping subtraction still leaves the right value.
Wide DivideWide(Wide dividend, Wide divisor, Wide* remainder)
{
    if (dividend.high == 0 && divisor.high == 0)
    {
        *remainder = {.high = 0, .low = dividend.low % divisor.low};
        return {.high = 0, .low = dividend.low / divisor.low};
    }
    *remainder = dividend;
    if (dividend.high == 0 && dividend.low == 0)
    {
        return dividend;
    }
    constexpr int HalfBits = 64;
    int bit = dividend.high != 0 ? 2 * HalfBits - 1 - __builtin_clzll(dividend.high)
                                 : HalfBits - 1 - __builtin_clzll(dividend.low);
    Wide quotient{.high = 0, .low = 0};
    Wide rest{.high = 0, .low = 0};
    for (; bit >= 0; --bit)
    {
        const bool carry = (rest.high >> (HalfBits - 1)) != 0;
        const uint64_t next =
            bit >= HalfBits ? dividend.high >> (bit - HalfBits) : dividend.low >> bit;
        rest.high = (rest.high << 1) | (rest.low >> (HalfBits - 1));
        rest.low = (rest.low << 1) | (next & 1);
        if (carry || AtLeast(rest, divisor))
        {
            rest = Subtract(rest, divisor);
            if (bit >= HalfBits)
            {
                quotient.high |= 1ULL << (bit - HalfBits);
            }
            else
            {
                quotient.low |= 1ULL << bit;
            }
        }
    }
    *remainder = rest;
    return quotient;
}

} // namespace FeatherOS::LibC
