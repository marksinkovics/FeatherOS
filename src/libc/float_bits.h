#ifndef FEATHEROS_LIBC_FLOAT_BITS_H
#define FEATHEROS_LIBC_FLOAT_BITS_H

#include <stdint.h>

namespace FeatherOS::LibC
{

using Mantissa = unsigned __int128;

// A binary floating-point format. Values are mantissa * 2^exponent; a normal value's leading
// mantissa bit sits at exponents from minimum_exponent to maximum_exponent.
struct FloatLayout
{
    int precision;
    int minimum_exponent;
    int maximum_exponent;
    int exponent_bits;
    // The x87 80-bit format stores its leading mantissa bit; the others imply it.
    bool explicit_leading_bit;
};

constexpr FloatLayout FloatFormat = {24, -126, 127, 8, false};
constexpr FloatLayout DoubleFormat = {53, -1022, 1023, 11, false};
constexpr FloatLayout X87Format = {64, -16382, 16383, 15, true};
constexpr FloatLayout QuadFormat = {113, -16382, 16383, 15, false};

#if __LDBL_MANT_DIG__ == 64
constexpr FloatLayout LongDoubleFormat = X87Format;
#elif __LDBL_MANT_DIG__ == 113
constexpr FloatLayout LongDoubleFormat = QuadFormat;
#else
constexpr FloatLayout LongDoubleFormat = DoubleFormat;
#endif

enum class FloatKind : uint8_t
{
    Zero,
    Finite,
    Infinite,
    NotANumber,
};

struct DecodedFloat
{
    FloatKind kind;
    bool negative;
    Mantissa mantissa;
    int exponent;
};

// The raw bits of a value, least significant first, in up to 128 bits.
template <typename Value> Mantissa RawBits(Value value)
{
    Mantissa bits = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    // An x87 long double occupies 16 bytes, but only its low 10 hold the value.
    const unsigned used =
        sizeof(Value) > 10 && LongDoubleFormat.explicit_leading_bit ? 10 : sizeof(Value);
    for (unsigned index = 0; index < used && index < 16; ++index)
    {
        bits |= static_cast<Mantissa>(bytes[index]) << (8 * index);
    }
    return bits;
}

template <typename Value> Value FromRawBits(Mantissa bits)
{
    Value value{};
    auto* bytes = reinterpret_cast<uint8_t*>(&value);
    for (unsigned index = 0; index < sizeof(Value) && index < 16; ++index)
    {
        bytes[index] = static_cast<uint8_t>(bits >> (8 * index));
    }
    return value;
}

DecodedFloat Decode(const FloatLayout& layout, Mantissa bits);

// mantissa has at most precision bits. biased is 0 for subnormals and zero, and all ones for
// infinity, whose mantissa is 0.
Mantissa Encode(const FloatLayout& layout, bool negative, Mantissa mantissa, int biased);

} // namespace FeatherOS::LibC

#endif
