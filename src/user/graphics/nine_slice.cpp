#include "nine_slice.h"

#include "corner_mask.h"
#include "surface.h"

#include <utility>
#include <vector>

namespace FeatherOS::Graphics
{

namespace
{

int32_t Clamp(int32_t value, int32_t low, int32_t high)
{
    return value < low ? low : (value > high ? high : value);
}

int32_t Saturate(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX
                             : (value < INT32_MIN ? INT32_MIN : static_cast<int32_t>(value));
}

int32_t HalfWidth(int32_t blur)
{
    return Clamp(blur, 0, MaximumShadowBlur) / 2;
}

// Zero outside the line; the window is 2 * half + 1 wide.
void BoxLine(const uint8_t* in, uint8_t* out, int32_t length, int32_t half)
{
    const uint32_t width = 2 * static_cast<uint32_t>(half) + 1;
    uint32_t sum = 0;
    for (int32_t i = 0; i < half && i < length; ++i)
    {
        sum += in[i];
    }
    for (int32_t i = 0; i < length; ++i)
    {
        if (i + half < length)
        {
            sum += in[i + half];
        }
        if (i - half - 1 >= 0)
        {
            sum -= in[i - half - 1];
        }
        out[i] = static_cast<uint8_t>((sum + width / 2) / width);
    }
}

void BoxHorizontal(const Mask& in, Mask& out, int32_t half)
{
    for (int32_t y = 0; y < in.Height(); ++y)
    {
        const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(in.Width());
        BoxLine(in.Data() + offset, out.Data() + offset, in.Width(), half);
    }
}

void BoxVertical(const Mask& in, Mask& out, int32_t half, std::vector<uint32_t>& sums)
{
    const auto width = static_cast<size_t>(in.Width());
    const uint32_t window = 2 * static_cast<uint32_t>(half) + 1;
    sums.assign(width, 0);
    for (int32_t y = 0; y < half && y < in.Height(); ++y)
    {
        for (size_t x = 0; x < width; ++x)
        {
            sums[x] += in.Data()[static_cast<size_t>(y) * width + x];
        }
    }
    for (int32_t y = 0; y < in.Height(); ++y)
    {
        const bool add = y + half < in.Height();
        const bool remove = y - half - 1 >= 0;
        const uint8_t* added = add ? in.Data() + static_cast<size_t>(y + half) * width : nullptr;
        const uint8_t* removed =
            remove ? in.Data() + static_cast<size_t>(y - half - 1) * width : nullptr;
        uint8_t* line = out.Data() + static_cast<size_t>(y) * width;
        for (size_t x = 0; x < width; ++x)
        {
            if (add)
            {
                sums[x] += added[x];
            }
            if (remove)
            {
                sums[x] -= removed[x];
            }
            line[x] = static_cast<uint8_t>((sums[x] + window / 2) / window);
        }
    }
}

} // namespace

int32_t ShadowSpread(int32_t blur)
{
    return 3 * HalfWidth(blur);
}

Mask MakeShadowMask(int32_t box_width, int32_t box_height, int32_t radius, int32_t blur)
{
    const int32_t spread = ShadowSpread(blur);
    if (box_width <= 0 || box_height <= 0 || box_width > MaximumDimension - 2 * spread ||
        box_height > MaximumDimension - 2 * spread)
    {
        return Mask(0, 0);
    }
    const int32_t smaller = box_width < box_height ? box_width : box_height;
    const int32_t r = Clamp(radius, 0, MaximumShadowRadius) < smaller / 2
                          ? Clamp(radius, 0, MaximumShadowRadius)
                          : smaller / 2;
    Mask mask(box_width + 2 * spread, box_height + 2 * spread);
    if (!mask.Valid())
    {
        return mask;
    }
    std::vector<uint8_t> corner(static_cast<size_t>(r > 0 ? r : 1));
    for (int32_t y = 0; y < box_height; ++y)
    {
        uint8_t* line = mask.Data() +
                        static_cast<size_t>(y + spread) * static_cast<size_t>(mask.Width()) +
                        static_cast<size_t>(spread);
        for (int32_t x = 0; x < box_width; ++x)
        {
            line[x] = 255;
        }
        const int32_t edge_row = y < r ? y : (y >= box_height - r ? box_height - 1 - y : r);
        if (edge_row < r)
        {
            ComputeCornerRow(r, edge_row, corner.data());
            for (int32_t x = 0; x < r; ++x)
            {
                line[x] = corner[static_cast<size_t>(x)];
                line[box_width - 1 - x] = corner[static_cast<size_t>(x)];
            }
        }
    }
    const int32_t half = HalfWidth(blur);
    if (half == 0)
    {
        return mask;
    }
    Mask scratch(mask.Width(), mask.Height());
    std::vector<uint32_t> sums;
    for (int pass = 0; pass < 3; ++pass)
    {
        BoxHorizontal(mask, scratch, half);
        std::swap(mask, scratch);
    }
    for (int pass = 0; pass < 3; ++pass)
    {
        BoxVertical(mask, scratch, half, sums);
        std::swap(mask, scratch);
    }
    return mask;
}

NineSlice MakeShadow(int32_t corner_radius, int32_t blur)
{
    const int32_t radius = Clamp(corner_radius, 0, MaximumShadowRadius);
    const int32_t inset = radius + 2 * ShadowSpread(blur);
    const int32_t box = 2 * radius + 2 * ShadowSpread(blur) + 1;
    return {MakeShadowMask(box, box, radius, blur), inset, inset, inset, inset};
}

Rectangle ShadowBounds(const Rectangle& box, int32_t offset_y, int32_t blur)
{
    const int64_t spread = ShadowSpread(blur);
    return {Saturate(static_cast<int64_t>(box.x) - spread),
            Saturate(static_cast<int64_t>(box.y) - spread + offset_y),
            Saturate(static_cast<int64_t>(box.width) + 2 * spread),
            Saturate(static_cast<int64_t>(box.height) + 2 * spread)};
}

} // namespace FeatherOS::Graphics
