#include "corner_mask.h"

#include "surface.h"

#include <atomic>
#include <memory>

namespace FeatherOS::Graphics
{

namespace
{

constexpr int64_t SamplesPerSide = 8;
constexpr int64_t SubPixels = 16;

uint64_t IntegerSqrt(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > value)
    {
        bit >>= 2;
    }
    while (bit != 0)
    {
        if (value >= result + bit)
        {
            value -= result + bit;
            result = (result >> 1) + bit;
        }
        else
        {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

uint8_t CoverageOfCount(int64_t count)
{
    return static_cast<uint8_t>((count * 255 + 32) / 64);
}

std::atomic<const Mask*>* CacheSlots()
{
    static std::atomic<const Mask*> slots[CornerCoverage::CachedRadiusLimit + 1];
    return slots;
}

const Mask* CachedMask(int32_t radius)
{
    std::atomic<const Mask*>& slot = CacheSlots()[radius];
    const Mask* mask = slot.load(std::memory_order_acquire);
    if (mask != nullptr)
    {
        return mask;
    }
    auto built = std::make_unique<Mask>(radius, radius);
    for (int32_t row = 0; row < radius; ++row)
    {
        ComputeCornerRow(radius, row, built->Data() + static_cast<size_t>(row) * radius);
    }
    // A thread that loses the race frees its copy; the masks are never freed otherwise.
    const Mask* expected = nullptr;
    if (slot.compare_exchange_strong(expected, built.get(), std::memory_order_acq_rel))
    {
        return built.release();
    }
    return expected;
}

} // namespace

void ComputeCornerRow(int32_t radius, int32_t row, uint8_t* out)
{
    const int64_t centre = SubPixels * radius;
    int64_t threshold[SamplesPerSide];
    int64_t lowest = centre;
    int64_t highest = 0;
    for (int64_t j = 0; j < SamplesPerSide; ++j)
    {
        const int64_t dy = centre - (SubPixels * row + 2 * j + 1);
        const auto reach =
            static_cast<int64_t>(IntegerSqrt(static_cast<uint64_t>(centre * centre - dy * dy)));
        // A sample at x is inside when x >= centre - reach.
        threshold[j] = centre - reach;
        lowest = threshold[j] < lowest ? threshold[j] : lowest;
        highest = threshold[j] > highest ? threshold[j] : highest;
    }
    const int64_t first_partial = lowest / SubPixels;
    const int64_t first_full = (highest + SubPixels - 2) / SubPixels;
    for (int64_t column = 0; column < radius; ++column)
    {
        if (column < first_partial)
        {
            out[column] = 0;
            continue;
        }
        if (column >= first_full)
        {
            out[column] = 255;
            continue;
        }
        int64_t count = 0;
        for (int64_t j = 0; j < SamplesPerSide; ++j)
        {
            const int64_t gap = threshold[j] - SubPixels * column - 1;
            if (gap <= 0)
            {
                count += SamplesPerSide;
            }
            else if ((gap + 1) / 2 < SamplesPerSide)
            {
                count += SamplesPerSide - (gap + 1) / 2;
            }
        }
        out[column] = CoverageOfCount(count);
    }
}

CornerCoverage::CornerCoverage(int32_t radius) : m_radius(radius), m_cached(nullptr)
{
    if (radius <= 0 || radius > MaximumDimension / 2)
    {
        m_radius = 0;
    }
    else if (radius <= CachedRadiusLimit)
    {
        m_cached = CachedMask(radius);
    }
    else
    {
        m_row.resize(static_cast<size_t>(radius));
    }
}

int32_t CornerCoverage::Radius() const
{
    return m_radius;
}

bool CornerCoverage::Cached() const
{
    return m_cached != nullptr;
}

const uint8_t* CornerCoverage::Row(int32_t row)
{
    if (row < 0 || row >= m_radius)
    {
        return nullptr;
    }
    if (m_cached != nullptr)
    {
        return m_cached->Data() + static_cast<size_t>(row) * static_cast<size_t>(m_radius);
    }
    ComputeCornerRow(m_radius, row, m_row.data());
    return m_row.data();
}

} // namespace FeatherOS::Graphics
