#include <catch2/catch_test_macros.hpp>

#include "libc/wide_division.h"

#include <cstdint>

namespace
{

using FeatherOS::LibC::Wide;

using Unsigned = unsigned __int128;

Wide Split(Unsigned value)
{
    return {.high = static_cast<uint64_t>(value >> 64), .low = static_cast<uint64_t>(value)};
}

Unsigned Join(Wide value)
{
    return static_cast<Unsigned>(value.high) << 64 | value.low;
}

void RequireMatchesHost(Unsigned dividend, Unsigned divisor)
{
    Wide remainder{};
    const Unsigned quotient =
        Join(FeatherOS::LibC::DivideWide(Split(dividend), Split(divisor), &remainder));
    INFO("dividend " << static_cast<uint64_t>(dividend >> 64) << ":"
                     << static_cast<uint64_t>(dividend) << ", divisor "
                     << static_cast<uint64_t>(divisor >> 64) << ":"
                     << static_cast<uint64_t>(divisor));
    REQUIRE(quotient == dividend / divisor);
    REQUIRE(Join(remainder) == dividend % divisor);
}

} // namespace

TEST_CASE("wide division matches the host's 128-bit division")
{
    const Unsigned all = ~static_cast<Unsigned>(0);
    const Unsigned values[] = {
        1,
        2,
        3,
        7,
        UINT64_MAX,
        static_cast<Unsigned>(UINT64_MAX) + 1,
        static_cast<Unsigned>(1) << 127,
        (static_cast<Unsigned>(1) << 127) + 1,
        all,
        all - 1,
        all / 3,
        static_cast<Unsigned>(0x123456789ABCDEF0ULL) << 64 | 0x0FEDCBA987654321ULL,
        static_cast<Unsigned>(10000000000000000000ULL) * 10000000000000000000ULL,
    };
    for (const Unsigned dividend : values)
    {
        RequireMatchesHost(0, dividend);
        for (const Unsigned divisor : values)
        {
            RequireMatchesHost(dividend, divisor);
        }
    }
}

TEST_CASE("wide division matches the host on a pseudo-random sweep")
{
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int index = 0; index < 20000; ++index)
    {
        const Unsigned dividend = static_cast<Unsigned>(next()) << 64 | next();
        // Divisors of every width, so both halves and the fast path are covered.
        Unsigned divisor = (static_cast<Unsigned>(next()) << 64 | next()) >> (next() % 128);
        if (divisor == 0)
        {
            divisor = 1;
        }
        RequireMatchesHost(dividend >> (next() % 128), divisor);
    }
}
