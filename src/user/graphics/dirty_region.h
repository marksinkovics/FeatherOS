#ifndef FEATHEROS_USER_GRAPHICS_DIRTY_REGION_H
#define FEATHEROS_USER_GRAPHICS_DIRTY_REGION_H

#include <stdint.h>

namespace FeatherOS::Graphics
{

struct Rectangle
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

// Rectangles that overlap or touch are merged into their bounding box. Past Capacity rectangles,
// or once they cover half the bounds, the region becomes the whole of the bounds.
class DirtyRegion
{
public:
    static constexpr uint32_t Capacity = 8;

    DirtyRegion(int32_t width, int32_t height);

    void Add(int32_t x, int32_t y, int32_t width, int32_t height);
    void AddAll();
    void Clear();
    uint32_t Count() const;
    Rectangle At(uint32_t index) const;

private:
    void Append(const Rectangle& rectangle);
    void Remove(uint32_t index);
    int64_t Area() const;

    Rectangle m_rectangles[Capacity];
    uint32_t m_count;
    int32_t m_width;
    int32_t m_height;
};

} // namespace FeatherOS::Graphics

#endif
