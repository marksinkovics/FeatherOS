#include "outline_renderer.h"

#include "blend.h"
#include "surface.h"

#include <vector>

namespace FeatherOS::Graphics
{

namespace
{

using Wide = unsigned __int128;

constexpr int64_t SamplesPerSide = 8;
// One sample is 256 sub-units wide; sample s has its centre at 256 s + 128.
constexpr int64_t SubUnits = 256;
constexpr int64_t Centre = 128;
constexpr int64_t Unbounded = int64_t{1} << 60;

int64_t Min(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

int64_t Max(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

int64_t FloorDiv(int64_t numerator, int64_t divisor)
{
    int64_t quotient = numerator / divisor;
    if (numerator % divisor != 0 && numerator < 0)
    {
        --quotient;
    }
    return quotient;
}

int64_t CeilDiv(int64_t numerator, int64_t divisor)
{
    return -FloorDiv(-numerator, divisor);
}

// The floor of the square root, by the bit-pair method: no division.
uint64_t IntegerSqrt(Wide value)
{
    if (value == 0)
    {
        return 0;
    }
    const auto high = static_cast<uint64_t>(value >> 64);
    const int leading =
        high != 0 ? __builtin_clzll(high) : 64 + __builtin_clzll(static_cast<uint64_t>(value));
    Wide result = 0;
    Wide bit = Wide{1} << ((127 - leading) & ~1);
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
    return static_cast<uint64_t>(result);
}

struct Mapped
{
    int64_t x;
    int64_t y;
};

class Rasterizer
{
public:
    Rasterizer(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
               const OutlineOptions& options, const OutlineWindow& window)
        : m_outlines(outlines), m_index(index), m_width(width), m_height(height),
          m_options(options), m_window(window), m_columns(window.right - window.left),
          m_samples(static_cast<int64_t>(m_columns) * SamplesPerSide),
          m_view(outlines.ViewBox(index)), m_delta(static_cast<size_t>(m_samples), 0),
          m_touched(static_cast<size_t>((m_samples + 63) / 64), 0),
          m_count(static_cast<size_t>(m_columns), 0),
          m_spread(static_cast<size_t>(m_columns) + 1, 0),
          m_coverage(static_cast<size_t>(m_columns), 0)
    {
        const int64_t smaller = Min(m_view.width, m_view.height);
        m_widest = 2 * 256 * smaller;
        // Strokes keep their roundness under a stretched view box by following the smaller scale.
        if (int64_t{m_width} * m_view.height <= int64_t{m_height} * m_view.width)
        {
            m_stroke_numerator = SamplesPerSide * m_width;
            m_stroke_denominator = m_view.width;
        }
        else
        {
            m_stroke_numerator = SamplesPerSide * m_height;
            m_stroke_denominator = m_view.height;
        }
    }

    size_t WorkingBytes() const
    {
        return m_delta.capacity() * sizeof(int32_t) + m_touched.capacity() * sizeof(uint64_t) +
               m_count.capacity() * sizeof(int32_t) + m_spread.capacity() * sizeof(int32_t) +
               m_coverage.capacity() + m_segments.capacity() * sizeof(Segment) +
               m_items.capacity() * sizeof(ItemInfo) + m_active.capacity() * sizeof(uint32_t);
    }

    uint64_t SegmentTests() const
    {
        return m_segment_tests;
    }

    void Run(OutlineRowFunction row, void* context)
    {
        Build();
        for (int32_t y = m_window.top; y < m_window.bottom; ++y)
        {
            const int64_t first = int64_t{y} * SamplesPerSide * SubUnits + Centre;
            const int64_t last = first + (SamplesPerSide - 1) * SubUnits;
            for (const ItemInfo& info : m_items)
            {
                if (info.high < first || info.low > last || !Select(info, first, last))
                {
                    continue;
                }
                if (RasterizeRow(info, y))
                {
                    size_t cursor = info.cursor;
                    OutlineItemView item;
                    if (m_outlines.NextItem(m_index, &cursor, &item))
                    {
                        row(context, item, m_window.left, y, m_coverage.data(), m_columns);
                    }
                }
            }
        }
    }

private:
    struct Segment
    {
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;
    };

    // One item: its segments, the stroke radius and the rows of centres its samples can reach.
    struct ItemInfo
    {
        size_t cursor;
        uint32_t first;
        uint32_t count;
        int64_t radius;
        int64_t low;
        int64_t high;
        bool stroke;
    };

    Mapped Map(OutlinePoint point) const
    {
        // Round to nearest, halves up: a point is p / 256 of a unit and a unit is
        // destination / view box pixels of 8 samples of 256 sub-units each. The results stay
        // within 2^29 for the loader's point range and the destination limit.
        return {
            FloorDiv(int64_t{point.x} * m_width * 16 + m_view.width, 2 * int64_t{m_view.width}),
            FloorDiv(int64_t{point.y} * m_height * 16 + m_view.height, 2 * int64_t{m_view.height})};
    }

    int64_t StrokeRadius(const OutlineItemView& item) const
    {
        int64_t width = m_options.stroke_width > 0 ? m_options.stroke_width : item.width;
        width = Min(width, m_widest);
        return FloorDiv(width * m_stroke_numerator + m_stroke_denominator,
                        2 * m_stroke_denominator);
    }

    // Maps every point once. A fill keeps its edges that are not level and closes each polyline;
    // a stroke keeps every segment, plus the closing one of a closed polyline.
    void Build()
    {
        size_t points = 0;
        {
            size_t cursor = 0;
            OutlineItemView item;
            while (m_outlines.NextItem(m_index, &cursor, &item))
            {
                OutlinePolylineView line;
                while (item.NextPolyline(&line))
                {
                    points += line.Count();
                }
            }
        }
        m_segments.reserve(points);
        m_items.reserve(m_outlines.ItemCount(m_index));
        size_t cursor = 0;
        OutlineItemView item;
        for (;;)
        {
            const size_t start = cursor;
            if (!m_outlines.NextItem(m_index, &cursor, &item))
            {
                break;
            }
            ItemInfo info{start,      static_cast<uint32_t>(m_segments.size()),
                          0,          item.stroke ? StrokeRadius(item) : 0,
                          INT64_MAX,  INT64_MIN,
                          item.stroke};
            OutlinePolylineView line;
            while (item.NextPolyline(&line))
            {
                Mapped first = Map(line.At(0));
                Mapped previous = first;
                for (uint32_t at = 1; at <= line.Count(); ++at)
                {
                    const bool closing = at == line.Count();
                    if (closing && item.stroke && !line.Closed())
                    {
                        break;
                    }
                    const Mapped current = closing ? first : Map(line.At(at));
                    if (item.stroke || previous.y != current.y)
                    {
                        m_segments.push_back(
                            {static_cast<int32_t>(previous.x), static_cast<int32_t>(previous.y),
                             static_cast<int32_t>(current.x), static_cast<int32_t>(current.y)});
                        info.low = Min(info.low, Min(previous.y, current.y));
                        info.high = Max(info.high, Max(previous.y, current.y));
                    }
                    previous = current;
                }
            }
            info.count = static_cast<uint32_t>(m_segments.size()) - info.first;
            if (info.count == 0 || (item.stroke && info.radius == 0))
            {
                continue;
            }
            info.low -= info.radius;
            info.high += info.radius;
            m_items.push_back(info);
        }
        m_active.reserve(m_segments.size());
    }

    // Lists the item's segments that can reach a sample centre from `first` to `last`.
    bool Select(const ItemInfo& info, int64_t first, int64_t last)
    {
        m_active.clear();
        for (uint32_t at = info.first; at < info.first + info.count; ++at)
        {
            const Segment& segment = m_segments[at];
            const int64_t low = Min(segment.y0, segment.y1) - info.radius;
            const int64_t high = Max(segment.y0, segment.y1) + info.radius;
            if (high >= first && low <= last)
            {
                m_active.push_back(at);
            }
        }
        return !m_active.empty();
    }

    // True when some sample of the pixel row is inside the item; leaves the row in m_coverage.
    bool RasterizeRow(const ItemInfo& info, int32_t y)
    {
        m_any = false;
        for (int64_t k = 0; k < SamplesPerSide; ++k)
        {
            const int64_t centre = (int64_t{y} * SamplesPerSide + k) * SubUnits + Centre;
            m_segment_tests += m_active.size();
            for (uint32_t at : m_active)
            {
                const Segment& segment = m_segments[at];
                const Mapped from{segment.x0, segment.y0};
                const Mapped to{segment.x1, segment.y1};
                if (info.stroke)
                {
                    Capsule(from, to, info.radius, centre);
                }
                else
                {
                    Crossing(from, to, centre);
                }
            }
            Flush();
        }
        if (!m_any)
        {
            return false;
        }
        int32_t running = 0;
        for (size_t x = 0; x < static_cast<size_t>(m_columns); ++x)
        {
            running += m_spread[x];
            const int32_t total = m_count[x] + running;
            m_coverage[x] = static_cast<uint8_t>((total * 255 + 32) / 64);
            m_count[x] = 0;
            m_spread[x] = 0;
        }
        m_spread[static_cast<size_t>(m_columns)] = 0;
        return true;
    }

    void Touch(int64_t column, int32_t delta)
    {
        const auto at = static_cast<size_t>(column);
        m_delta[at] += delta;
        m_touched[at >> 6] |= uint64_t{1} << (at & 63);
        m_low = at >> 6 < m_low ? at >> 6 : m_low;
        m_high = at >> 6 > m_high ? at >> 6 : m_high;
    }

    // Winding changes at the first sample whose centre is not left of `x`.
    void StepAt(int64_t x, int32_t delta)
    {
        const int64_t column =
            FloorDiv(x + SubUnits - Centre - 1, SubUnits) - int64_t{m_window.left} * SamplesPerSide;
        if (column >= m_samples)
        {
            return;
        }
        Touch(Max(column, 0), delta);
    }

    // Adds the samples whose centres lie in [first, last], both in sub-units.
    void Cover(int64_t first, int64_t last)
    {
        const int64_t base = int64_t{m_window.left} * SamplesPerSide;
        const int64_t from = FloorDiv(first + SubUnits - Centre - 1, SubUnits) - base;
        const int64_t to = FloorDiv(last - Centre, SubUnits) - base;
        if (to < 0 || from >= m_samples || from > to)
        {
            return;
        }
        Touch(Max(from, 0), 1);
        if (to + 1 < m_samples)
        {
            Touch(to + 1, -1);
        }
    }

    void Crossing(Mapped from, Mapped to, int64_t centre)
    {
        int32_t direction = 1;
        if (from.y > to.y)
        {
            const Mapped swap = from;
            from = to;
            to = swap;
            direction = -1;
        }
        if (centre < from.y || centre >= to.y)
        {
            return;
        }
        const int64_t rise = to.y - from.y;
        StepAt(from.x + CeilDiv((centre - from.y) * (to.x - from.x), rise), direction);
    }

    void Disc(Mapped at, int64_t radius, int64_t centre)
    {
        const int64_t rise = centre - at.y;
        if (rise > radius || rise < -radius)
        {
            return;
        }
        const auto half =
            static_cast<int64_t>(IntegerSqrt(static_cast<Wide>(radius * radius - rise * rise)));
        Cover(at.x - half, at.x + half);
    }

    // The samples of this row within `radius` of the segment, as the round-capped strip's
    // interval joined with the two end discs'. Every compare is on integers: the strip's
    // |cross| <= radius * length becomes |cross| <= floor(sqrt(radius^2 * length^2)).
    void Capsule(Mapped from, Mapped to, int64_t radius, int64_t centre)
    {
        if (centre < Min(from.y, to.y) - radius || centre > Max(from.y, to.y) + radius)
        {
            return;
        }
        Disc(from, radius, centre);
        const int64_t dx = to.x - from.x;
        const int64_t dy = to.y - from.y;
        if (dx == 0 && dy == 0)
        {
            return;
        }
        Disc(to, radius, centre);
        const int64_t length2 = dx * dx + dy * dy;
        const auto reach = static_cast<int64_t>(IntegerSqrt(
            static_cast<Wide>(radius) * static_cast<Wide>(radius) * static_cast<Wide>(length2)));
        const int64_t v = centre - from.y;
        int64_t low = -Unbounded;
        int64_t high = Unbounded;
        // |u * dy - v * dx| <= reach
        if (dy > 0)
        {
            low = CeilDiv(v * dx - reach, dy);
            high = FloorDiv(v * dx + reach, dy);
        }
        else if (dy < 0)
        {
            low = CeilDiv(-v * dx - reach, -dy);
            high = FloorDiv(-v * dx + reach, -dy);
        }
        else if (v * dx > reach || v * dx < -reach)
        {
            return;
        }
        // 0 <= u * dx + v * dy <= length2
        const int64_t along_low = -v * dy;
        const int64_t along_high = length2 - v * dy;
        if (dx > 0)
        {
            low = Max(low, CeilDiv(along_low, dx));
            high = Min(high, FloorDiv(along_high, dx));
        }
        else if (dx < 0)
        {
            low = Max(low, CeilDiv(-along_high, -dx));
            high = Min(high, FloorDiv(-along_low, -dx));
        }
        else if (along_low > 0 || along_high < 0)
        {
            return;
        }
        if (low <= high)
        {
            Cover(from.x + low, from.x + high);
        }
    }

    void Flush()
    {
        if (m_low > m_high)
        {
            return;
        }
        int32_t winding = 0;
        bool inside = false;
        int64_t start = 0;
        for (size_t word = m_low; word <= m_high; ++word)
        {
            uint64_t bits = m_touched[word];
            m_touched[word] = 0;
            while (bits != 0)
            {
                const int64_t column = static_cast<int64_t>(word * 64) + __builtin_ctzll(bits);
                bits &= bits - 1;
                winding += m_delta[static_cast<size_t>(column)];
                m_delta[static_cast<size_t>(column)] = 0;
                const bool now = winding != 0;
                if (now && !inside)
                {
                    start = column;
                }
                else if (!now && inside)
                {
                    Accumulate(start, column);
                }
                inside = now;
            }
        }
        if (inside)
        {
            Accumulate(start, m_samples);
        }
        m_low = SIZE_MAX;
        m_high = 0;
    }

    // Samples [from, to) of one sample row are inside: partial pixels at the ends, whole ones in
    // between through the difference array.
    void Accumulate(int64_t from, int64_t to)
    {
        m_any = true;
        const auto first = static_cast<size_t>(from / SamplesPerSide);
        const auto last = static_cast<size_t>((to - 1) / SamplesPerSide);
        if (first == last)
        {
            m_count[first] += static_cast<int32_t>(to - from);
            return;
        }
        m_count[first] += static_cast<int32_t>((first + 1) * SamplesPerSide - from);
        m_count[last] += static_cast<int32_t>(to - static_cast<int64_t>(last) * SamplesPerSide);
        if (last > first + 1)
        {
            m_spread[first + 1] += SamplesPerSide;
            m_spread[last] -= SamplesPerSide;
        }
    }

    const Outlines& m_outlines;
    int32_t m_index;
    int32_t m_width;
    int32_t m_height;
    OutlineOptions m_options;
    OutlineWindow m_window;
    int32_t m_columns;
    int64_t m_samples;
    OutlineSize m_view;
    std::vector<int32_t> m_delta;
    std::vector<uint64_t> m_touched;
    std::vector<int32_t> m_count;
    std::vector<int32_t> m_spread;
    std::vector<uint8_t> m_coverage;
    int64_t m_widest = 0;
    int64_t m_stroke_numerator = 1;
    int64_t m_stroke_denominator = 1;
    std::vector<Segment> m_segments;
    std::vector<ItemInfo> m_items;
    std::vector<uint32_t> m_active;
    uint64_t m_segment_tests = 0;
    size_t m_low = SIZE_MAX;
    size_t m_high = 0;
    bool m_any = false;
};

struct MaskTarget
{
    Mask* mask;
};

void CompositeIntoMask(void* context, const OutlineItemView& item, int32_t x, int32_t y,
                       const uint8_t* coverage, int32_t count)
{
    Mask* mask = static_cast<MaskTarget*>(context)->mask;
    uint8_t* line = mask->Data() + static_cast<size_t>(y) * static_cast<size_t>(mask->Width()) +
                    static_cast<size_t>(x);
    for (int32_t column = 0; column < count; ++column)
    {
        if (coverage[column] == 0)
        {
            continue;
        }
        const uint32_t alpha =
            item.opacity == 255 ? coverage[column] : ScaleChannel(coverage[column], item.opacity);
        line[column] = static_cast<uint8_t>(OverChannel(alpha, line[column], alpha));
    }
}

} // namespace

size_t RasterizeOutline(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
                        const OutlineOptions& options, const OutlineWindow& window,
                        OutlineRowFunction row, void* context, uint64_t* segment_tests)
{
    if (segment_tests != nullptr)
    {
        *segment_tests = 0;
    }
    if (index < 0 || index >= outlines.Count() || width < 1 || height < 1 ||
        width > MaximumOutlineSize || height > MaximumOutlineSize || window.left < 0 ||
        window.top < 0 || window.left >= window.right || window.top >= window.bottom ||
        window.right > width || window.bottom > height ||
        window.right - window.left > MaximumDimension)
    {
        return 0;
    }
    Rasterizer rasterizer(outlines, index, width, height, options, window);
    rasterizer.Run(row, context);
    if (segment_tests != nullptr)
    {
        *segment_tests = rasterizer.SegmentTests();
    }
    return rasterizer.WorkingBytes();
}

Mask RenderOutlineMask(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
                       const OutlineOptions& options, size_t* working_bytes)
{
    if (working_bytes != nullptr)
    {
        *working_bytes = 0;
    }
    Mask mask(width, height);
    if (!mask.Valid() || !outlines.SinglePaint(index))
    {
        return Mask(0, 0);
    }
    MaskTarget target{&mask};
    const size_t used = RasterizeOutline(outlines, index, width, height, options,
                                         {0, 0, width, height}, CompositeIntoMask, &target);
    if (working_bytes != nullptr)
    {
        *working_bytes = used;
    }
    return mask;
}

} // namespace FeatherOS::Graphics
