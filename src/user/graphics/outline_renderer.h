#ifndef FEATHEROS_USER_GRAPHICS_OUTLINE_RENDERER_H
#define FEATHEROS_USER_GRAPHICS_OUTLINE_RENDERER_H

#include "color.h"
#include "mask.h"
#include "outlines.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Graphics
{

struct OutlinePaints
{
    Color current = 0xFF000000U;
    Color slots[Outlines::Slots] = {};
};

struct OutlineOptions
{
    // In 1/256 of a unit; above 0 it replaces the width of every stroke.
    int32_t stroke_width = 0;

    bool operator==(const OutlineOptions&) const = default;
};

// The part of a drawing's destination to rasterize, in pixels from the destination's top-left.
struct OutlineWindow
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

// A destination side above this draws nothing; it keeps every product inside 128 bits.
constexpr int32_t MaximumOutlineSize = 65536;

// Receives one pixel row of one item's coverage (0 to 255) at a time, in drawing order; the
// bytes are valid until the call returns.
using OutlineRowFunction = void (*)(void* context, const OutlineItemView& item, int32_t x,
                                    int32_t y, const uint8_t* coverage, int32_t count);

// Maps the view box onto a `width` x `height` destination and rasterizes `window` of it with an 8x8
// sample grid per pixel. Integer arithmetic only. Works row by row, so its memory depends on the
// window's width and the drawing's points (at most MaximumDrawingPoints), never on the height;
// returns the bytes of working memory it allocated, 0 if it drew nothing. Every point is mapped
// once, and a row visits only the segments that can reach it; `segment_tests`, when given, takes
// the number of segment evaluations made, which is the work that scales with the drawing.
size_t RasterizeOutline(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
                        const OutlineOptions& options, const OutlineWindow& window,
                        OutlineRowFunction row, void* context, uint64_t* segment_tests = nullptr);

// The coverage of a drawing whose items all use `Current`, items composited in order; an invalid
// mask for any other drawing or size.
Mask RenderOutlineMask(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
                       const OutlineOptions& options = {}, size_t* working_bytes = nullptr);

} // namespace FeatherOS::Graphics

#endif
