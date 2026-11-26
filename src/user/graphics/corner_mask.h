#ifndef FEATHEROS_USER_GRAPHICS_CORNER_MASK_H
#define FEATHEROS_USER_GRAPHICS_CORNER_MASK_H

#include "mask.h"

#include <stdint.h>

#include <vector>

namespace FeatherOS::Graphics
{

// Coverage of a quarter disc for the top-left corner of a rounded rectangle: the disc's centre is
// the bottom-right corner of the radius x radius square. 8x8 samples per pixel, sample centres at
// (2k + 1) / 16, tested with squared integer distances. A radius of 0 to MaximumDimension / 2 is
// accepted; `row` is 0 to radius - 1 and `out` takes `radius` bytes.
void ComputeCornerRow(int32_t radius, int32_t row, uint8_t* out);

// Rows of one radius. Radii up to CachedRadiusLimit share a process-wide mask built once; a larger
// radius is computed row by row, so memory stays at one row however big the corner is.
class CornerCoverage
{
public:
    static constexpr int32_t CachedRadiusLimit = 64;

    explicit CornerCoverage(int32_t radius);

    int32_t Radius() const;
    // Valid until the next call.
    const uint8_t* Row(int32_t row);
    bool Cached() const;

private:
    int32_t m_radius;
    const Mask* m_cached;
    std::vector<uint8_t> m_row;
};

} // namespace FeatherOS::Graphics

#endif
