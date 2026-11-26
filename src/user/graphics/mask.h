#ifndef FEATHEROS_USER_GRAPHICS_MASK_H
#define FEATHEROS_USER_GRAPHICS_MASK_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace FeatherOS::Graphics
{

// One byte of coverage per pixel, 0 to 255. A size outside 1 to MaximumDimension gives an
// invalid, empty mask.
class Mask
{
public:
    Mask(int32_t width, int32_t height);

    bool Valid() const;
    int32_t Width() const;
    int32_t Height() const;
    uint8_t* Data();
    const uint8_t* Data() const;
    uint8_t CoverageAt(int32_t x, int32_t y) const;
    void SetCoverage(int32_t x, int32_t y, uint8_t coverage);

private:
    std::vector<uint8_t> m_coverage;
    int32_t m_width;
    int32_t m_height;
};

} // namespace FeatherOS::Graphics

#endif
