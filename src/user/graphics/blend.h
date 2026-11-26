#ifndef FEATHEROS_USER_GRAPHICS_BLEND_H
#define FEATHEROS_USER_GRAPHICS_BLEND_H

#include <stdint.h>

namespace FeatherOS::Graphics
{

// The only rounding in a blend; the rule Text::Blend uses.
constexpr uint32_t Div255(uint32_t x)
{
    return (x + 127) / 255;
}

// Straight source over any destination: Div255(s * a + d * (255 - a)).
constexpr uint32_t MixChannel(uint32_t source, uint32_t destination, uint32_t alpha)
{
    return Div255(source * alpha + destination * (255 - alpha));
}

// Premultiplied source over any destination. The cap only matters for a source whose channel
// exceeds its alpha, which would otherwise carry into the next channel.
constexpr uint32_t OverChannel(uint32_t source, uint32_t destination, uint32_t alpha)
{
    const uint32_t result = source + Div255(destination * (255 - alpha));
    return result > 255 ? 255 : result;
}

constexpr uint32_t ScaleChannel(uint32_t value, uint32_t factor)
{
    return Div255(value * factor);
}

constexpr uint32_t MixPixel(uint32_t destination, uint32_t source, uint32_t alpha)
{
    uint32_t result = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        result |= MixChannel((source >> shift) & 0xFFU, (destination >> shift) & 0xFFU, alpha)
                  << shift;
    }
    return result;
}

// Premultiplied ARGB source over an XRGB destination; the result's top byte is 0.
constexpr uint32_t OverXrgb(uint32_t destination, uint32_t source)
{
    const uint32_t alpha = source >> 24;
    uint32_t result = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        result |= OverChannel((source >> shift) & 0xFFU, (destination >> shift) & 0xFFU, alpha)
                  << shift;
    }
    return result;
}

// Premultiplied ARGB over premultiplied ARGB: the alpha channel follows the same rule.
constexpr uint32_t OverArgb(uint32_t destination, uint32_t source)
{
    const uint32_t alpha = source >> 24;
    uint32_t result = 0;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        result |= OverChannel((source >> shift) & 0xFFU, (destination >> shift) & 0xFFU, alpha)
                  << shift;
    }
    return result;
}

constexpr uint32_t ScalePixel(uint32_t pixel, uint32_t factor)
{
    uint32_t result = 0;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        result |= ScaleChannel((pixel >> shift) & 0xFFU, factor) << shift;
    }
    return result;
}

} // namespace FeatherOS::Graphics

#endif
