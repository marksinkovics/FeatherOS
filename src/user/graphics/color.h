#ifndef FEATHEROS_USER_GRAPHICS_COLOR_H
#define FEATHEROS_USER_GRAPHICS_COLOR_H

#include "blend.h"

#include <stdint.h>

namespace FeatherOS::Graphics
{

// 0xAARRGGBB with straight alpha.
using Color = uint32_t;

constexpr Color Rgb(uint32_t red, uint32_t green, uint32_t blue)
{
    return 0xFF000000U | (red & 0xFFU) << 16 | (green & 0xFFU) << 8 | (blue & 0xFFU);
}

constexpr Color WithAlpha(Color color, uint32_t alpha)
{
    return (color & 0x00FFFFFFU) | (alpha & 0xFFU) << 24;
}

// Colour channels scaled by the alpha; the alpha stays.
constexpr Color Premultiply(Color color)
{
    return (ScalePixel(color, color >> 24) & 0x00FFFFFFU) | (color & 0xFF000000U);
}

} // namespace FeatherOS::Graphics

#endif
