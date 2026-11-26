#ifndef FEATHEROS_USER_GRAPHICS_REGION_H
#define FEATHEROS_USER_GRAPHICS_REGION_H

#include "dirty_region.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace FeatherOS::Graphics
{

// Edges are clamped to +-Limit (the largest surface), so any 32-bit input is safe.
// Canonical form: rectangles sorted by y then x; those with the same y and height form a band;
// bands do not overlap; rectangles of a band neither overlap nor touch; vertically touching
// bands with equal spans are one band.
class Region
{
public:
    static constexpr int32_t Limit = 16384;

    Region() = default;
    explicit Region(const Rectangle& rectangle);

    void Add(const Rectangle& rectangle);
    void Add(const Region& other);
    void Subtract(const Rectangle& rectangle);
    void Subtract(const Region& other);
    void Intersect(const Rectangle& rectangle);
    void Intersect(const Region& other);
    void Translate(int32_t dx, int32_t dy);
    void Clear();

    bool Empty() const;
    Rectangle Bounds() const;
    bool Contains(int32_t x, int32_t y) const;
    size_t RectangleCount() const;
    Rectangle RectangleAt(size_t index) const;

    bool operator==(const Region& other) const;

private:
    enum class Operation
    {
        Union,
        Subtract,
        Intersect,
    };

    void Combine(const Region& other, Operation operation);

    std::vector<Rectangle> m_rectangles;
};

} // namespace FeatherOS::Graphics

#endif
