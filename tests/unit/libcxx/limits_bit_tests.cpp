#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/bit"
#include "libcxx/include/cctype"
#include "libcxx/include/cinttypes"
#include "libcxx/include/climits"
#include "libcxx/include/cstddef"
#include "libcxx/include/cstdint"
#include "libcxx/include/cstdio"
#include "libcxx/include/cstdlib"
#include "libcxx/include/cstring"
#include "libcxx/include/ctime"
#include "libcxx/include/limits"

#include <cfloat>

namespace
{

namespace fstd = featheros_std;

// Keeps a value from being folded into a constant, so the checks run the generated code.
template <class T> T Opaque(T value)
{
    volatile T copy = value;
    return copy;
}

} // namespace

TEST_CASE("numeric_limits describes integers", "[libcxx]")
{
    STATIC_REQUIRE(fstd::numeric_limits<int>::max() == INT_MAX &&
                   fstd::numeric_limits<int>::min() == INT_MIN);
    STATIC_REQUIRE(fstd::numeric_limits<unsigned long>::max() == ULONG_MAX &&
                   fstd::numeric_limits<unsigned long>::min() == 0);
    STATIC_REQUIRE(fstd::numeric_limits<signed char>::lowest() == SCHAR_MIN);
    STATIC_REQUIRE(fstd::numeric_limits<char>::is_signed == (CHAR_MIN < 0));
    STATIC_REQUIRE(fstd::numeric_limits<bool>::digits == 1 && fstd::numeric_limits<bool>::max() &&
                   !fstd::numeric_limits<bool>::is_modulo);
    STATIC_REQUIRE(fstd::numeric_limits<short>::digits == 15 &&
                   fstd::numeric_limits<short>::digits10 == 4);
    STATIC_REQUIRE(fstd::numeric_limits<unsigned long long>::digits10 == 19 &&
                   fstd::numeric_limits<long long>::digits10 == 18);
    STATIC_REQUIRE(fstd::numeric_limits<__int128>::digits == 127 &&
                   fstd::numeric_limits<unsigned __int128>::digits10 == 38);
    STATIC_REQUIRE(fstd::numeric_limits<char16_t>::max() == 0xFFFF);
    STATIC_REQUIRE(fstd::numeric_limits<unsigned>::is_modulo && !fstd::numeric_limits<int>::is_modulo);
    STATIC_REQUIRE(fstd::numeric_limits<const volatile int>::max() == INT_MAX);
    STATIC_REQUIRE(!fstd::numeric_limits<int*>::is_specialized);
}

TEST_CASE("numeric_limits describes floating point", "[libcxx]")
{
    using Float = fstd::numeric_limits<float>;
    using Double = fstd::numeric_limits<double>;
    STATIC_REQUIRE(Float::max() == FLT_MAX && Float::min() == FLT_MIN && Float::lowest() == -FLT_MAX);
    STATIC_REQUIRE(Double::digits == 53 && Double::digits10 == 15 && Double::max_digits10 == 17);
    STATIC_REQUIRE(Float::max_digits10 == 9 && Double::epsilon() == DBL_EPSILON);
    STATIC_REQUIRE(Double::min_exponent == DBL_MIN_EXP && Double::max_exponent10 == DBL_MAX_10_EXP);
    STATIC_REQUIRE(Double::is_iec559 && Double::has_infinity && Double::has_signaling_NaN);
    STATIC_REQUIRE(Double::round_style == fstd::round_to_nearest && Double::round_error() == 0.5);
    REQUIRE(Opaque(Double::infinity()) > Double::max());
    REQUIRE(Opaque(Double::quiet_NaN()) != Double::quiet_NaN());
    REQUIRE(Opaque(Float::signaling_NaN()) != Float::signaling_NaN());
    REQUIRE((Opaque(Double::denorm_min()) > 0.0 && Double::denorm_min() < Double::min()));
    REQUIRE(fstd::numeric_limits<long double>::digits == LDBL_MANT_DIG);
}

TEST_CASE("bit counts and finds bits", "[libcxx]")
{
    STATIC_REQUIRE(fstd::popcount(0u) == 0 && fstd::popcount(0xFFFF'FFFF'FFFF'FFFFull) == 64);
    STATIC_REQUIRE(fstd::countl_zero(uint8_t{0}) == 8 && fstd::countl_zero(uint16_t{1}) == 15);
    STATIC_REQUIRE(fstd::countl_zero(uint64_t{1} << 63) == 0);
    STATIC_REQUIRE(fstd::countr_zero(uint32_t{0}) == 32 && fstd::countr_zero(uint32_t{8}) == 3);
    STATIC_REQUIRE(fstd::countl_one(uint8_t{0xF0}) == 4 && fstd::countr_one(uint8_t{0x07}) == 3);
    constexpr unsigned __int128 wide = static_cast<unsigned __int128>(1) << 70;
    STATIC_REQUIRE(fstd::countl_zero(wide) == 57 && fstd::countr_zero(wide) == 70 &&
                   fstd::popcount(wide | 1) == 2);
    STATIC_REQUIRE(fstd::countl_zero(static_cast<unsigned __int128>(1)) == 127);
    REQUIRE(fstd::popcount(Opaque(0x8000'0000'0000'0001ull)) == 2);
    REQUIRE(fstd::countl_zero(Opaque(uint32_t{0x10})) == 27);
    REQUIRE(fstd::countr_zero(Opaque(uint64_t{0})) == 64);
}

TEST_CASE("bit finds powers of two and rotates", "[libcxx]")
{
    STATIC_REQUIRE(fstd::has_single_bit(64u) && !fstd::has_single_bit(0u) &&
                   !fstd::has_single_bit(6u));
    STATIC_REQUIRE(fstd::bit_width(0u) == 0 && fstd::bit_width(255u) == 8);
    STATIC_REQUIRE(fstd::bit_floor(0u) == 0 && fstd::bit_floor(255u) == 128);
    STATIC_REQUIRE(fstd::bit_ceil(0u) == 1 && fstd::bit_ceil(1u) == 1 && fstd::bit_ceil(129u) == 256);
    STATIC_REQUIRE(fstd::bit_ceil(uint8_t{128}) == 128);
    STATIC_REQUIRE(fstd::rotl(uint8_t{0x81}, 1) == 0x03 && fstd::rotl(uint8_t{0x81}, -1) == 0xC0);
    STATIC_REQUIRE(fstd::rotr(uint16_t{1}, 1) == 0x8000 && fstd::rotr(uint16_t{1}, 17) == 0x8000);
    STATIC_REQUIRE(fstd::rotl(0x1234u, 0) == 0x1234u);
}

TEST_CASE("bit swaps bytes and casts", "[libcxx]")
{
    STATIC_REQUIRE(fstd::byteswap(uint8_t{0x12}) == 0x12);
    STATIC_REQUIRE(fstd::byteswap(int16_t{0x0102}) == 0x0201);
    STATIC_REQUIRE(fstd::byteswap(uint64_t{0x0102'0304'0506'0708}) == 0x0807'0605'0403'0201);
    constexpr unsigned __int128 wide = (static_cast<unsigned __int128>(0x0102) << 112) | 0x0304;
    STATIC_REQUIRE(fstd::byteswap(wide) == ((static_cast<unsigned __int128>(0x0403) << 112) | 0x0201));
    STATIC_REQUIRE(fstd::bit_cast<uint64_t>(1.0) == 0x3FF0'0000'0000'0000);
    STATIC_REQUIRE(fstd::bit_cast<float>(0x4000'0000u) == 2.0f);
    STATIC_REQUIRE(fstd::endian::native == fstd::endian::little);
}

TEST_CASE("cstddef has byte", "[libcxx]")
{
    constexpr fstd::byte value{0x0F};
    STATIC_REQUIRE(fstd::to_integer<int>(value << 4) == 0xF0);
    STATIC_REQUIRE(fstd::to_integer<unsigned>(~value) == 0xF0);
    STATIC_REQUIRE(fstd::to_integer<int>((value | fstd::byte{0x30}) & fstd::byte{0x3C}) == 0x3C);
    fstd::byte shifted{1};
    shifted <<= 3;
    shifted ^= fstd::byte{0xFF};
    REQUIRE(fstd::to_integer<int>(shifted) == 0xF7);
    STATIC_REQUIRE(fstd::is_same_v<fstd::size_t, decltype(sizeof(0))>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::nullptr_t, decltype(nullptr)>);
}

TEST_CASE("the C wrappers bring the C library into the namespace", "[libcxx]")
{
    char text[] = "feather";
    const char* constant = text;
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::strchr(text, 'e')), char*>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::strchr(constant, 'e')), const char*>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::memchr(constant, 'e', 1)), const void*>);
    REQUIRE(fstd::strrchr(text, 'e') == text + 5);
    REQUIRE(fstd::strpbrk(constant, "ht") == constant + 3);
    REQUIRE(fstd::strstr(text, "her") == text + 4);
    REQUIRE(fstd::strlen(text) == 7);

    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::abs(1L)), long>);
    REQUIRE((fstd::abs(Opaque(-2)) == 2 && fstd::abs(Opaque(-3LL)) == 3));
    REQUIRE((fstd::abs(Opaque(-1.5f)) == 1.5f && fstd::abs(Opaque(-0.25)) == 0.25));
    const fstd::lldiv_t quotient = fstd::div(Opaque(-7LL), 2LL);
    REQUIRE((quotient.quot == -3 && quotient.rem == -1));
    REQUIRE(fstd::strtoll("-12", nullptr, 10) == -12);
    REQUIRE(fstd::strtoimax("34", nullptr, 10) == 34);
    REQUIRE(fstd::tolower('Q') == 'q');
    char buffer[8];
    REQUIRE(fstd::snprintf(buffer, sizeof(buffer), "%03d", 7) == 3);
    REQUIRE(fstd::strcmp(buffer, "007") == 0);
    fstd::time_t epoch = 0;
    REQUIRE(fstd::gmtime(&epoch)->tm_year == 70);
    STATIC_REQUIRE(fstd::is_same_v<fstd::uint32_t, uint32_t>);
}
