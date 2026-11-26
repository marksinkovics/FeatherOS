#include "canvas.h"

#include "blend.h"
#include "font.h"
#include "../text/utf8.h"

#include <string.h>

#include <vector>

namespace FeatherOS::Graphics
{

namespace
{

// Far beyond anything a 32-bit coordinate can reach in a test or a program, and far from
// overflowing the 64-bit sums made with it.
constexpr int64_t OriginLimit = int64_t{1} << 50;

int64_t Max(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

int64_t Min(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

int32_t Saturate(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX
                             : (value < INT32_MIN ? INT32_MIN : static_cast<int32_t>(value));
}

int64_t ClampOrigin(int64_t value)
{
    return Max(-OriginLimit, Min(OriginLimit, value));
}

uint32_t Opaque(Color color, bool premultiplied)
{
    return premultiplied ? (color | 0xFF000000U) : (color & ColorMask);
}

uint32_t BlendColor(uint32_t destination, Color color, uint32_t alpha, bool premultiplied)
{
    if (alpha == 0)
    {
        return destination;
    }
    if (alpha == 255)
    {
        return Opaque(color, premultiplied);
    }
    if (premultiplied)
    {
        return OverArgb(destination, Premultiply(WithAlpha(color, alpha)));
    }
    return MixPixel(destination, color, alpha);
}

uint32_t CompositePixel(uint32_t destination, uint32_t source, bool source_premultiplied,
                        bool destination_premultiplied, uint32_t opacity)
{
    if (!source_premultiplied)
    {
        if (opacity == 255)
        {
            return Opaque(source, destination_premultiplied);
        }
        if (destination_premultiplied)
        {
            return OverArgb(destination, ScalePixel(source | 0xFF000000U, opacity));
        }
        return MixPixel(destination, source, opacity);
    }
    const uint32_t scaled = opacity == 255 ? source : ScalePixel(source, opacity);
    const uint32_t alpha = scaled >> 24;
    if (alpha == 0)
    {
        return destination;
    }
    if (alpha == 255)
    {
        return destination_premultiplied ? scaled : (scaled & ColorMask);
    }
    return destination_premultiplied ? OverArgb(destination, scaled)
                                     : OverXrgb(destination, scaled);
}

void PaintPixel(uint32_t& pixel, Color color, uint32_t color_alpha, uint32_t coverage,
                bool premultiplied)
{
    const uint32_t alpha = coverage == 255 ? color_alpha : ScaleChannel(color_alpha, coverage);
    pixel = BlendColor(pixel, color, alpha, premultiplied);
}

size_t Index(int64_t row, int64_t column, int32_t width)
{
    return static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(column);
}

struct OutlineTarget
{
    Surface* surface;
    const OutlinePaints* paints;
    int64_t left;
    int64_t top;
    bool premultiplied;
};

void PaintOutlineRow(void* context, const OutlineItemView& item, int32_t x, int32_t y,
                     const uint8_t* coverage, int32_t count)
{
    const auto& target = *static_cast<const OutlineTarget*>(context);
    Color color = target.paints->current;
    if (item.paint == OutlinePaintKind::Fixed)
    {
        color = item.color;
    }
    else if (item.paint == OutlinePaintKind::Slot)
    {
        color = target.paints->slots[item.slot % Outlines::Slots];
    }
    uint32_t alpha = color >> 24;
    if (item.opacity != 255)
    {
        alpha = ScaleChannel(alpha, item.opacity);
    }
    if (alpha == 0)
    {
        return;
    }
    color = WithAlpha(color, alpha);
    uint32_t* line =
        target.surface->Pixels() + Index(y + target.top, x + target.left, target.surface->Width());
    for (int32_t column = 0; column < count; ++column)
    {
        if (coverage[column] != 0)
        {
            PaintPixel(line[column], color, alpha, coverage[column], target.premultiplied);
        }
    }
}

// The source pixels [first, last) that destination index `index` of `count` covers.
void SourceRange(int64_t index, int64_t count, int64_t size, int64_t* first, int64_t* last)
{
    if (count >= size)
    {
        *first = (2 * index + 1) * size / (2 * count);
        *last = *first + 1;
        return;
    }
    *first = index * size / count;
    *last = (index + 1) * size / count;
}

constexpr uint32_t Ellipsis = 0x2026;

} // namespace

Canvas::Canvas(Surface& surface)
    : m_surface(surface), m_state{0,
                                  0,
                                  0,
                                  0,
                                  surface.Valid() ? surface.Width() : 0,
                                  surface.Valid() ? surface.Height() : 0},
      m_saved{}, m_depth(0), m_refused(0)
{
}

bool Canvas::Save()
{
    if (m_depth == MaximumDepth)
    {
        ++m_refused;
        return false;
    }
    m_saved[m_depth++] = m_state;
    return true;
}

void Canvas::Restore()
{
    if (m_refused > 0)
    {
        --m_refused;
    }
    else if (m_depth > 0)
    {
        m_state = m_saved[--m_depth];
    }
}

void Canvas::ClipTo(const Rectangle& rectangle)
{
    const int64_t x = static_cast<int64_t>(rectangle.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(rectangle.y) + m_state.origin_y;
    const int64_t left = Max(m_state.left, x);
    const int64_t top = Max(m_state.top, y);
    const int64_t right = Min(m_state.right, x + rectangle.width);
    const int64_t bottom = Min(m_state.bottom, y + rectangle.height);
    if (rectangle.width <= 0 || rectangle.height <= 0 || left >= right || top >= bottom)
    {
        m_state.left = m_state.top = m_state.right = m_state.bottom = 0;
        return;
    }
    m_state.left = static_cast<int32_t>(left);
    m_state.top = static_cast<int32_t>(top);
    m_state.right = static_cast<int32_t>(right);
    m_state.bottom = static_cast<int32_t>(bottom);
}

void Canvas::Translate(int32_t dx, int32_t dy)
{
    m_state.origin_x = ClampOrigin(m_state.origin_x + dx);
    m_state.origin_y = ClampOrigin(m_state.origin_y + dy);
}

Surface& Canvas::Target() const
{
    return m_surface;
}

Rectangle Canvas::Clip() const
{
    return {m_state.left, m_state.top, m_state.right - m_state.left, m_state.bottom - m_state.top};
}

int64_t Canvas::OriginX() const
{
    return m_state.origin_x;
}

int64_t Canvas::OriginY() const
{
    return m_state.origin_y;
}

Canvas::Bounds Canvas::Visible(int64_t x, int64_t y, int64_t width, int64_t height) const
{
    if (!m_surface.Valid() || width <= 0 || height <= 0)
    {
        return {0, 0, 0, 0};
    }
    return {Max(m_state.left, x), Max(m_state.top, y), Min(m_state.right, x + width),
            Min(m_state.bottom, y + height)};
}

void Canvas::FillRectangle(const Rectangle& rectangle, Color color)
{
    FillArea(static_cast<int64_t>(rectangle.x) + m_state.origin_x,
             static_cast<int64_t>(rectangle.y) + m_state.origin_y, rectangle.width,
             rectangle.height, color);
}

void Canvas::FillArea(int64_t x, int64_t y, int64_t width, int64_t height, Color color)
{
    const uint32_t alpha = color >> 24;
    if (alpha == 0)
    {
        return;
    }
    const Bounds area = Visible(x, y, width, height);
    if (area.Empty())
    {
        return;
    }
    const bool premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    const int32_t stride = m_surface.Width();
    for (int64_t row = area.top; row < area.bottom; ++row)
    {
        uint32_t* line = m_surface.Pixels() + Index(row, area.left, stride);
        const auto count = static_cast<size_t>(area.right - area.left);
        if (alpha == 255)
        {
            const uint32_t pixel = Opaque(color, premultiplied);
            for (size_t column = 0; column < count; ++column)
            {
                line[column] = pixel;
            }
            continue;
        }
        for (size_t column = 0; column < count; ++column)
        {
            line[column] = BlendColor(line[column], color, alpha, premultiplied);
        }
    }
}

void Canvas::DrawSurface(const Surface& source, int32_t x, int32_t y)
{
    Composite(source, 0, 0, source.Width(), source.Height(), x, y, 255);
}

void Canvas::DrawSurfacePart(const Surface& source, int32_t sx, int32_t sy, int32_t width,
                             int32_t height, int32_t x, int32_t y)
{
    Composite(source, sx, sy, width, height, x, y, 255);
}

void Canvas::DrawSurfaceFaded(const Surface& source, int32_t x, int32_t y, uint8_t opacity)
{
    Composite(source, 0, 0, source.Width(), source.Height(), x, y, opacity);
}

void Canvas::Composite(const Surface& source, int32_t sx, int32_t sy, int32_t width, int32_t height,
                       int32_t x, int32_t y, uint32_t opacity)
{
    if (!m_surface.Valid() || !source.Valid() || &source == &m_surface || width <= 0 ||
        height <= 0 || opacity == 0)
    {
        return;
    }
    // The part is cut to the source first; the position moves by what was cut off.
    const int64_t part_left = Max(sx, 0);
    const int64_t part_top = Max(sy, 0);
    const int64_t part_right = Min(static_cast<int64_t>(sx) + width, source.Width());
    const int64_t part_bottom = Min(static_cast<int64_t>(sy) + height, source.Height());
    if (part_left >= part_right || part_top >= part_bottom)
    {
        return;
    }
    const int64_t origin_x = static_cast<int64_t>(x) + m_state.origin_x + (part_left - sx);
    const int64_t origin_y = static_cast<int64_t>(y) + m_state.origin_y + (part_top - sy);
    const Bounds area = Visible(origin_x, origin_y, part_right - part_left, part_bottom - part_top);
    if (area.Empty())
    {
        return;
    }
    const bool source_premultiplied = source.Format() == PixelFormat::PremultipliedArgb;
    const bool destination_premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    const auto count = static_cast<size_t>(area.right - area.left);
    for (int64_t row = area.top; row < area.bottom; ++row)
    {
        const uint32_t* from =
            source.Pixels() +
            Index(part_top + (row - origin_y), part_left + (area.left - origin_x), source.Width());
        uint32_t* to = m_surface.Pixels() + Index(row, area.left, m_surface.Width());
        if (!source_premultiplied && !destination_premultiplied && opacity == 255)
        {
            memcpy(to, from, count * sizeof(uint32_t));
            continue;
        }
        for (size_t column = 0; column < count; ++column)
        {
            to[column] = CompositePixel(to[column], from[column], source_premultiplied,
                                        destination_premultiplied, opacity);
        }
    }
}

void Canvas::DrawSurfaceScaled(const Surface& source, const Rectangle& destination)
{
    if (!m_surface.Valid() || !source.Valid() || &source == &m_surface || destination.width <= 0 ||
        destination.height <= 0)
    {
        return;
    }
    const int64_t x = static_cast<int64_t>(destination.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(destination.y) + m_state.origin_y;
    const Bounds area = Visible(x, y, destination.width, destination.height);
    if (area.Empty())
    {
        return;
    }
    const bool source_premultiplied = source.Format() == PixelFormat::PremultipliedArgb;
    const bool destination_premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    const auto count = static_cast<size_t>(area.right - area.left);
    std::vector<int64_t> first(count);
    std::vector<int64_t> last(count);
    for (size_t column = 0; column < count; ++column)
    {
        SourceRange(area.left - x + static_cast<int64_t>(column), destination.width, source.Width(),
                    &first[column], &last[column]);
    }
    for (int64_t row = area.top; row < area.bottom; ++row)
    {
        int64_t top = 0;
        int64_t bottom = 0;
        SourceRange(row - y, destination.height, source.Height(), &top, &bottom);
        uint32_t* to = m_surface.Pixels() + Index(row, area.left, m_surface.Width());
        for (size_t column = 0; column < count; ++column)
        {
            const uint64_t pixels = static_cast<uint64_t>(bottom - top) *
                                    static_cast<uint64_t>(last[column] - first[column]);
            uint64_t sums[4] = {0, 0, 0, 0};
            for (int64_t sy = top; sy < bottom; ++sy)
            {
                const uint32_t* from = source.Pixels() + Index(sy, first[column], source.Width());
                for (int64_t sx = first[column]; sx < last[column]; ++sx, ++from)
                {
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        sums[channel] += (*from >> (8 * channel)) & 0xFFU;
                    }
                }
            }
            uint32_t average = 0;
            for (int channel = 0; channel < 4; ++channel)
            {
                average |= static_cast<uint32_t>((sums[channel] + pixels / 2) / pixels)
                           << (8 * channel);
            }
            to[column] = CompositePixel(to[column], average, source_premultiplied,
                                        destination_premultiplied, 255);
        }
    }
}

void Canvas::DrawNineSlice(const NineSlice& slice, const Rectangle& outer, Color color,
                           bool skip_centre)
{
    const uint32_t alpha = color >> 24;
    const Mask& mask = slice.mask;
    if (alpha == 0 || !m_surface.Valid() || !mask.Valid() || outer.width <= 0 ||
        outer.height <= 0 || slice.left < 0 || slice.top < 0 || slice.right < 0 ||
        slice.bottom < 0 || static_cast<int64_t>(slice.left) + slice.right > mask.Width() ||
        static_cast<int64_t>(slice.top) + slice.bottom > mask.Height())
    {
        return;
    }
    const int64_t width = outer.width;
    const int64_t height = outer.height;
    int64_t left = slice.left;
    int64_t right = slice.right;
    if (left + right > width)
    {
        left = width * slice.left / (slice.left + slice.right);
        right = width - left;
    }
    int64_t top = slice.top;
    int64_t bottom = slice.bottom;
    if (top + bottom > height)
    {
        top = height * slice.top / (slice.top + slice.bottom);
        bottom = height - top;
    }
    const int64_t x = static_cast<int64_t>(outer.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(outer.y) + m_state.origin_y;
    const Bounds area = Visible(x, y, width, height);
    if (area.Empty())
    {
        return;
    }
    const int64_t mask_width = mask.Width();
    const int64_t mask_height = mask.Height();
    const int64_t middle_column =
        Min(mask_width - 1, slice.left + (mask_width - slice.left - slice.right) / 2);
    const int64_t middle_row =
        Min(mask_height - 1, slice.top + (mask_height - slice.top - slice.bottom) / 2);
    const bool premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    const int64_t body_left = x + left;
    const int64_t body_right = x + width - right;
    for (int64_t row = area.top; row < area.bottom; ++row)
    {
        const int64_t dy = row - y;
        const bool centre_row = dy >= top && dy < height - bottom;
        const int64_t source_row =
            dy < top ? dy : (centre_row ? middle_row : mask_height - (height - dy));
        const uint8_t* coverage =
            mask.Data() + static_cast<size_t>(source_row) * static_cast<size_t>(mask_width);
        uint32_t* line = m_surface.Pixels() + Index(row, 0, m_surface.Width());
        for (int64_t column = area.left; column < area.right; ++column)
        {
            const int64_t dx = column - x;
            int64_t source_column = 0;
            if (column < body_left)
            {
                source_column = dx;
            }
            else if (column < body_right)
            {
                if (skip_centre && centre_row)
                {
                    column = Min(body_right, area.right) - 1;
                    continue;
                }
                source_column = middle_column;
            }
            else
            {
                source_column = mask_width - (width - dx);
            }
            const uint32_t value = coverage[source_column];
            if (value != 0)
            {
                PaintPixel(line[column], color, alpha, value, premultiplied);
            }
        }
    }
}

int64_t Canvas::DrawRun(const Font& font, int64_t pen, int64_t y, const char* utf8, size_t length,
                        Color color)
{
    const Text::FontView& view = font.View();
    const uint32_t color_alpha = color >> 24;
    uint32_t alphas[16];
    for (uint32_t level = 0; level < 16; ++level)
    {
        alphas[level] = ScaleChannel(color_alpha, level * 17);
    }
    const bool premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    const bool visible = m_surface.Valid() && color_alpha != 0;
    const int64_t baseline = y + view.Ascent();
    for (size_t offset = 0; offset < length;)
    {
        size_t used = 0;
        const char32_t code_point = Text::DecodeNext(utf8 + offset, length - offset, &used);
        offset += used;
        if (code_point < 0x20 || code_point == 0x7F)
        {
            continue;
        }
        const Text::Glyph glyph = view.Find(code_point);
        const int64_t left = pen + glyph.left;
        const int64_t top = baseline - glyph.top;
        pen += glyph.advance;
        if (!visible)
        {
            continue;
        }
        const Bounds area = Visible(left, top, glyph.width, glyph.height);
        for (int64_t row = area.top; row < area.bottom; ++row)
        {
            uint32_t* line = m_surface.Pixels() + Index(row, 0, m_surface.Width());
            for (int64_t column = area.left; column < area.right; ++column)
            {
                const uint32_t level = view.Coverage(glyph, static_cast<int32_t>(column - left),
                                                     static_cast<int32_t>(row - top));
                if (level != 0)
                {
                    line[column] = BlendColor(line[column], color, alphas[level > 15 ? 15 : level],
                                              premultiplied);
                }
            }
        }
    }
    return pen;
}

int32_t Canvas::DrawText(const Font& font, int32_t x, int32_t y, const char* utf8, Color color)
{
    if (!font.Valid() || utf8 == nullptr)
    {
        return x;
    }
    const int64_t pen =
        DrawRun(font, static_cast<int64_t>(x) + m_state.origin_x,
                static_cast<int64_t>(y) + m_state.origin_y, utf8, strlen(utf8), color);
    return Saturate(pen - m_state.origin_x);
}

void Canvas::DrawTextEllipsis(const Font& font, const Rectangle& rectangle, const char* utf8,
                              Color color)
{
    if (!font.Valid() || utf8 == nullptr || rectangle.width <= 0 || rectangle.height <= 0)
    {
        return;
    }
    const char* dots = font.HasGlyph(Ellipsis) ? "\xE2\x80\xA6" : "...";
    const int64_t dots_width = font.MeasureText(dots);
    const size_t length = strlen(utf8);
    int64_t used_width = 0;
    int64_t prefix_width = 0;
    size_t prefix_end = 0;
    bool has_prefix = false;
    bool fits = true;
    for (size_t offset = 0; offset < length;)
    {
        if (used_width + dots_width <= rectangle.width)
        {
            prefix_end = offset;
            prefix_width = used_width;
            has_prefix = true;
        }
        size_t used = 0;
        const char32_t code_point = Text::DecodeNext(utf8 + offset, length - offset, &used);
        offset += used;
        used_width += font.Advance(code_point);
        if (used_width > rectangle.width)
        {
            fits = false;
            break;
        }
    }
    if (!fits && !has_prefix)
    {
        return;
    }
    const State saved = m_state;
    ClipTo(rectangle);
    const int64_t x = static_cast<int64_t>(rectangle.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(rectangle.y) + m_state.origin_y;
    if (fits)
    {
        DrawRun(font, x, y, utf8, length, color);
    }
    else
    {
        DrawRun(font, x, y, utf8, prefix_end, color);
        DrawRun(font, x + prefix_width, y, dots, strlen(dots), color);
    }
    m_state = saved;
}

void Canvas::DrawMask(const Mask& mask, int32_t x, int32_t y, Color color)
{
    const uint32_t color_alpha = color >> 24;
    if (color_alpha == 0 || !mask.Valid())
    {
        return;
    }
    const int64_t origin_x = static_cast<int64_t>(x) + m_state.origin_x;
    const int64_t origin_y = static_cast<int64_t>(y) + m_state.origin_y;
    const Bounds area = Visible(origin_x, origin_y, mask.Width(), mask.Height());
    if (area.Empty())
    {
        return;
    }
    const bool premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    for (int64_t row = area.top; row < area.bottom; ++row)
    {
        const uint8_t* coverage =
            mask.Data() + Index(row - origin_y, area.left - origin_x, mask.Width());
        uint32_t* line = m_surface.Pixels() + Index(row, area.left, m_surface.Width());
        const auto count = static_cast<size_t>(area.right - area.left);
        for (size_t column = 0; column < count; ++column)
        {
            if (coverage[column] != 0)
            {
                PaintPixel(line[column], color, color_alpha, coverage[column], premultiplied);
            }
        }
    }
}

void Canvas::DrawOutline(const Outlines& outlines, int32_t index, const Rectangle& destination,
                         const OutlinePaints& paints, const OutlineOptions& options)
{
    if (index < 0 || index >= outlines.Count() || destination.width > MaximumOutlineSize ||
        destination.height > MaximumOutlineSize)
    {
        return;
    }
    const int64_t x = static_cast<int64_t>(destination.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(destination.y) + m_state.origin_y;
    const Bounds area = Visible(x, y, destination.width, destination.height);
    if (area.Empty())
    {
        return;
    }
    OutlineTarget target{&m_surface, &paints, x, y,
                         m_surface.Format() == PixelFormat::PremultipliedArgb};
    const OutlineWindow window{
        static_cast<int32_t>(area.left - x), static_cast<int32_t>(area.top - y),
        static_cast<int32_t>(area.right - x), static_cast<int32_t>(area.bottom - y)};
    RasterizeOutline(outlines, index, destination.width, destination.height, options, window,
                     PaintOutlineRow, &target);
}

void Canvas::DrawHorizontalLine(int32_t x, int32_t y, int32_t length, Color color)
{
    FillArea(static_cast<int64_t>(x) + m_state.origin_x, static_cast<int64_t>(y) + m_state.origin_y,
             length, 1, color);
}

void Canvas::DrawVerticalLine(int32_t x, int32_t y, int32_t length, Color color)
{
    FillArea(static_cast<int64_t>(x) + m_state.origin_x, static_cast<int64_t>(y) + m_state.origin_y,
             1, length, color);
}

void Canvas::PaintCorner(CornerCoverage& outer, CornerCoverage* hole, int32_t inset, int64_t x,
                         int64_t y, bool mirror_x, bool mirror_y, Color color)
{
    const int64_t size = outer.Radius();
    const Bounds area = Visible(x, y, size, size);
    if (area.Empty())
    {
        return;
    }
    const uint32_t color_alpha = color >> 24;
    const bool premultiplied = m_surface.Format() == PixelFormat::PremultipliedArgb;
    const int64_t hole_size = hole != nullptr ? hole->Radius() : 0;
    for (int64_t row = area.top; row < area.bottom; ++row)
    {
        const int64_t corner_row = mirror_y ? size - 1 - (row - y) : row - y;
        const uint8_t* outer_row = outer.Row(static_cast<int32_t>(corner_row));
        const int64_t hole_row_index = corner_row - inset;
        const uint8_t* hole_row = hole_row_index >= 0 && hole_row_index < hole_size
                                      ? hole->Row(static_cast<int32_t>(hole_row_index))
                                      : nullptr;
        uint32_t* line = m_surface.Pixels() + Index(row, area.left, m_surface.Width());
        for (int64_t column = area.left; column < area.right; ++column, ++line)
        {
            const int64_t corner_column = mirror_x ? size - 1 - (column - x) : column - x;
            uint32_t coverage = outer_row[corner_column];
            const int64_t hole_column = corner_column - inset;
            if (hole_row != nullptr && hole_column >= 0 && hole_column < hole_size)
            {
                const uint32_t inner = hole_row[hole_column];
                coverage = coverage > inner ? coverage - inner : 0;
            }
            if (coverage != 0)
            {
                PaintPixel(*line, color, color_alpha, coverage, premultiplied);
            }
        }
    }
}

void Canvas::FillRoundedRectangle(const Rectangle& rectangle, int32_t radius, Color color)
{
    if (rectangle.width <= 0 || rectangle.height <= 0 || (color >> 24) == 0)
    {
        return;
    }
    const int64_t width = rectangle.width;
    const int64_t height = rectangle.height;
    const int64_t r = Max(0, Min(radius, Min(width, height) / 2));
    const int64_t x = static_cast<int64_t>(rectangle.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(rectangle.y) + m_state.origin_y;
    FillArea(x + r, y, width - 2 * r, r, color);
    FillArea(x, y + r, width, height - 2 * r, color);
    FillArea(x + r, y + height - r, width - 2 * r, r, color);
    if (r == 0)
    {
        return;
    }
    CornerCoverage corner(static_cast<int32_t>(r));
    PaintCorner(corner, nullptr, 0, x, y, false, false, color);
    PaintCorner(corner, nullptr, 0, x + width - r, y, true, false, color);
    PaintCorner(corner, nullptr, 0, x, y + height - r, false, true, color);
    PaintCorner(corner, nullptr, 0, x + width - r, y + height - r, true, true, color);
}

void Canvas::StrokeRoundedRectangle(const Rectangle& rectangle, int32_t radius, int32_t width,
                                    Color color)
{
    if (rectangle.width <= 0 || rectangle.height <= 0 || width <= 0 || (color >> 24) == 0)
    {
        return;
    }
    const int64_t full_width = rectangle.width;
    const int64_t full_height = rectangle.height;
    const int64_t smaller = Min(full_width, full_height);
    if (2 * static_cast<int64_t>(width) >= smaller)
    {
        FillRoundedRectangle(rectangle, radius, color);
        return;
    }
    const int64_t r = Max(0, Min(radius, smaller / 2));
    const int64_t w = width;
    const int64_t x = static_cast<int64_t>(rectangle.x) + m_state.origin_x;
    const int64_t y = static_cast<int64_t>(rectangle.y) + m_state.origin_y;
    const int64_t band = Min(w, r);
    const int64_t right = x + full_width;
    const int64_t bottom = y + full_height;
    // The straight edges between the corner squares, then whatever of the ring lies between them
    // when the stroke is thicker than the radius.
    FillArea(x + r, y, full_width - 2 * r, band, color);
    FillArea(x + r, bottom - band, full_width - 2 * r, band, color);
    FillArea(x, y + r, band, full_height - 2 * r, color);
    FillArea(right - band, y + r, band, full_height - 2 * r, color);
    if (w > r)
    {
        FillArea(x + r, y + r, full_width - 2 * r, w - r, color);
        FillArea(x + r, bottom - w, full_width - 2 * r, w - r, color);
        FillArea(x + r, y + w, w - r, full_height - 2 * w, color);
        FillArea(right - w, y + w, w - r, full_height - 2 * w, color);
    }
    if (r == 0)
    {
        return;
    }
    CornerCoverage outer(static_cast<int32_t>(r));
    CornerCoverage inner(static_cast<int32_t>(Max(r - w, 0)));
    const auto inset = static_cast<int32_t>(w);
    PaintCorner(outer, &inner, inset, x, y, false, false, color);
    PaintCorner(outer, &inner, inset, right - r, y, true, false, color);
    PaintCorner(outer, &inner, inset, x, bottom - r, false, true, color);
    PaintCorner(outer, &inner, inset, right - r, bottom - r, true, true, color);
}

} // namespace FeatherOS::Graphics
