#ifndef FEATHEROS_USER_GRAPHICS_NINE_SLICE_H
#define FEATHEROS_USER_GRAPHICS_NINE_SLICE_H

#include "dirty_region.h"
#include "mask.h"

#include <stdint.h>

namespace FeatherOS::Graphics
{

// A mask cut into nine parts by its four insets: the corners keep their size, the edges and the
// centre are stretched by repeating the middle row or column of their part.
struct NineSlice
{
    Mask mask;
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

constexpr int32_t MaximumShadowRadius = 128;
constexpr int32_t MaximumShadowBlur = 128;

// How far the blur reaches beyond the box: three box passes of half-width blur / 2. A blur is cut
// to 0..MaximumShadowBlur and a radius to 0..MaximumShadowRadius, which bounds the mask at about
// 1025 x 1025 bytes and every pass at the mask's pixel count.
int32_t ShadowSpread(int32_t blur);

// The mask of a rounded rectangle of `box_width` x `box_height`, blurred by three box passes of
// width `blur | 1` in each direction (each sum rounded to nearest), on a canvas
// ShadowSpread(blur) larger on every side. The radius is cut to half the smaller side.
Mask MakeShadowMask(int32_t box_width, int32_t box_height, int32_t radius, int32_t blur);

// A shadow whose slices stretch exactly to the blur of any larger box. The insets are the radius
// plus twice the spread: the blur of a straight edge only becomes constant there.
NineSlice MakeShadow(int32_t corner_radius, int32_t blur);

// The box grown by the spread on every side and moved down by offset_y, saturated to 32 bits.
Rectangle ShadowBounds(const Rectangle& box, int32_t offset_y, int32_t blur);

} // namespace FeatherOS::Graphics

#endif
