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

inline DecodedFloat Decode(const FloatLayout& layout, Mantissa bits)
{
    const int fraction_bits = layout.explicit_leading_bit ? layout.precision : layout.precision - 1;
    const Mantissa fraction = bits & ((static_cast<Mantissa>(1) << fraction_bits) - 1);
    const auto biased =
        static_cast<int>((bits >> fraction_bits) & ((1U << layout.exponent_bits) - 1));
    DecodedFloat decoded = {FloatKind::Finite,
                            ((bits >> (fraction_bits + layout.exponent_bits)) & 1) != 0, 0, 0};
    const int all_ones = (1 << layout.exponent_bits) - 1;
    const Mantissa payload = layout.explicit_leading_bit
                                 ? fraction & ~(static_cast<Mantissa>(1) << (fraction_bits - 1))
                                 : fraction;
    if (biased == all_ones)
    {
        decoded.kind = payload == 0 ? FloatKind::Infinite : FloatKind::NotANumber;
        return decoded;
    }
    if (biased == 0)
    {
        decoded.kind = fraction == 0 ? FloatKind::Zero : FloatKind::Finite;
        decoded.mantissa = fraction;
        decoded.exponent = layout.minimum_exponent - (layout.precision - 1);
        return decoded;
    }
    decoded.mantissa = layout.explicit_leading_bit
                           ? fraction
                           : fraction | (static_cast<Mantissa>(1) << fraction_bits);
    decoded.exponent = biased - layout.maximum_exponent - (layout.precision - 1);
    return decoded;
}

// mantissa has at most precision bits. biased is 0 for subnormals and zero, and all ones for
// infinity, whose mantissa is 0.
inline Mantissa Encode(const FloatLayout& layout, bool negative, Mantissa mantissa, int biased)
{
    const int fraction_bits = layout.explicit_leading_bit ? layout.precision : layout.precision - 1;
    Mantissa fraction = mantissa;
    if (!layout.explicit_leading_bit)
    {
        fraction &= (static_cast<Mantissa>(1) << fraction_bits) - 1;
    }
    else if (biased == (1 << layout.exponent_bits) - 1)
    {
        // The x87 infinity keeps its leading bit set.
        fraction = static_cast<Mantissa>(1) << (fraction_bits - 1);
    }
    return fraction | (static_cast<Mantissa>(biased) << fraction_bits) |
           (static_cast<Mantissa>(negative ? 1 : 0) << (fraction_bits + layout.exponent_bits));
}

} // namespace FeatherOS::LibC

#endif
