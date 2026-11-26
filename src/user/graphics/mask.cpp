#include "mask.h"

#include "surface.h"

namespace FeatherOS::Graphics
{

Mask::Mask(int32_t width, int32_t height) : m_width(0), m_height(0)
{
    if (width <= 0 || height <= 0 || width > MaximumDimension || height > MaximumDimension)
    {
        return;
    }
    m_coverage.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    m_width = width;
    m_height = height;
}

bool Mask::Valid() const
{
    return !m_coverage.empty();
}

int32_t Mask::Width() const
{
    return m_width;
}

int32_t Mask::Height() const
{
    return m_height;
}

uint8_t* Mask::Data()
{
    return m_coverage.data();
}

const uint8_t* Mask::Data() const
{
    return m_coverage.data();
}

uint8_t Mask::CoverageAt(int32_t x, int32_t y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
    {
        return 0;
    }
    return m_coverage[static_cast<size_t>(y) * static_cast<size_t>(m_width) +
                      static_cast<size_t>(x)];
}

void Mask::SetCoverage(int32_t x, int32_t y, uint8_t coverage)
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
    {
        return;
    }
    m_coverage[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)] =
        coverage;
}

} // namespace FeatherOS::Graphics
