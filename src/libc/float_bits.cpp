#include "float_bits.h"

#include <stdint.h>

namespace FeatherOS::LibC
{

DecodedFloat Decode(const FloatLayout& layout, Mantissa bits)
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

Mantissa Encode(const FloatLayout& layout, bool negative, Mantissa mantissa, int biased)
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
