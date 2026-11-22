#include "dirty_region.h"

namespace FeatherOS::Graphics
{

namespace
{

bool Touches(const Rectangle& first, const Rectangle& second)
{
    return first.x <= second.x + second.width && second.x <= first.x + first.width &&
           first.y <= second.y + second.height && second.y <= first.y + first.height;
}

Rectangle Union(const Rectangle& first, const Rectangle& second)
{
    const int32_t left = first.x < second.x ? first.x : second.x;
    const int32_t top = first.y < second.y ? first.y : second.y;
    const int32_t first_right = first.x + first.width;
    const int32_t second_right = second.x + second.width;
    const int32_t first_bottom = first.y + first.height;
    const int32_t second_bottom = second.y + second.height;
    const int32_t right = first_right > second_right ? first_right : second_right;
    const int32_t bottom = first_bottom > second_bottom ? first_bottom : second_bottom;
    return {left, top, right - left, bottom - top};
}

} // namespace

DirtyRegion::DirtyRegion(int32_t width, int32_t height)
    : m_rectangles{}, m_count(0), m_width(width), m_height(height)
{
}

void DirtyRegion::Add(int32_t x, int32_t y, int32_t width, int32_t height)
{
    const int64_t left = x < 0 ? 0 : x;
    const int64_t top = y < 0 ? 0 : y;
    int64_t right = static_cast<int64_t>(x) + width;
    int64_t bottom = static_cast<int64_t>(y) + height;
    right = right > m_width ? m_width : right;
    bottom = bottom > m_height ? m_height : bottom;
    if (width <= 0 || height <= 0 || left >= right || top >= bottom)
    {
        return;
    }
    Rectangle merged = {static_cast<int32_t>(left), static_cast<int32_t>(top),
                        static_cast<int32_t>(right - left), static_cast<int32_t>(bottom - top)};
    for (uint32_t index = 0; index < m_count;)
    {
        if (Touches(merged, m_rectangles[index]))
        {
            merged = Union(merged, m_rectangles[index]);
            Remove(index);
            index = 0;
        }
        else
        {
            ++index;
        }
    }
    Append(merged);
}

void DirtyRegion::AddAll()
{
    m_count = 0;
    if (m_width > 0 && m_height > 0)
    {
        m_rectangles[m_count++] = {0, 0, m_width, m_height};
    }
}

void DirtyRegion::Clear()
{
    m_count = 0;
}

uint32_t DirtyRegion::Count() const
{
    return m_count;
}

Rectangle DirtyRegion::At(uint32_t index) const
{
    return index < m_count ? m_rectangles[index] : Rectangle{0, 0, 0, 0};
}

void DirtyRegion::Append(const Rectangle& rectangle)
{
    if (m_count == Capacity)
    {
        AddAll();
        return;
    }
    m_rectangles[m_count++] = rectangle;
    if (Area() * 2 >= static_cast<int64_t>(m_width) * m_height)
    {
        AddAll();
    }
}

void DirtyRegion::Remove(uint32_t index)
{
    for (uint32_t next = index + 1; next < m_count; ++next)
    {
        m_rectangles[next - 1] = m_rectangles[next];
    }
    --m_count;
}

int64_t DirtyRegion::Area() const
{
    int64_t total = 0;
    for (uint32_t index = 0; index < m_count; ++index)
    {
        total += static_cast<int64_t>(m_rectangles[index].width) * m_rectangles[index].height;
    }
    return total;
}

} // namespace FeatherOS::Graphics
