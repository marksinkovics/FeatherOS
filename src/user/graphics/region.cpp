#include "region.h"

#include <algorithm>

// GCC -O2 cannot see that the vector's growth arithmetic never wraps, and rejects push_back below
// under -Werror (stringop-overflow, alloc-size-larger-than); Release builds failed on it.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Walloc-size-larger-than="
#endif

namespace FeatherOS::Graphics
{

namespace
{

struct Span
{
    int32_t start;
    int32_t end;
};

struct Band
{
    int32_t top;
    int32_t bottom;
    size_t first;
    size_t last;
};

int32_t Clamp(int64_t value)
{
    constexpr int64_t Limit = Region::Limit;
    return static_cast<int32_t>(value < -Limit ? -Limit : (value > Limit ? Limit : value));
}

std::vector<Band> BandsOf(const std::vector<Rectangle>& rectangles)
{
    std::vector<Band> bands;
    for (size_t index = 0; index < rectangles.size(); ++index)
    {
        const Rectangle& rectangle = rectangles[index];
        if (bands.empty() || bands.back().top != rectangle.y)
        {
            bands.push_back({rectangle.y, rectangle.y + rectangle.height, index, index + 1});
        }
        else
        {
            bands.back().last = index + 1;
        }
    }
    return bands;
}

void SpansOf(const std::vector<Rectangle>& rectangles, const Band& band, std::vector<Span>& spans)
{
    spans.clear();
    for (size_t index = band.first; index < band.last; ++index)
    {
        spans.push_back({rectangles[index].x, rectangles[index].x + rectangles[index].width});
    }
}

void UnionSpans(const std::vector<Span>& first, const std::vector<Span>& second,
                std::vector<Span>& out)
{
    size_t a = 0;
    size_t b = 0;
    while (a < first.size() || b < second.size())
    {
        const bool take_first =
            b == second.size() || (a < first.size() && first[a].start <= second[b].start);
        const Span next = take_first ? first[a++] : second[b++];
        if (!out.empty() && next.start <= out.back().end)
        {
            out.back().end = std::max(out.back().end, next.end);
        }
        else
        {
            out.push_back(next);
        }
    }
}

void IntersectSpans(const std::vector<Span>& first, const std::vector<Span>& second,
                    std::vector<Span>& out)
{
    size_t a = 0;
    size_t b = 0;
    while (a < first.size() && b < second.size())
    {
        const int32_t start = std::max(first[a].start, second[b].start);
        const int32_t end = std::min(first[a].end, second[b].end);
        if (start < end)
        {
            out.push_back({start, end});
        }
        if (first[a].end < second[b].end)
        {
            ++a;
        }
        else
        {
            ++b;
        }
    }
}

void SubtractSpans(const std::vector<Span>& first, const std::vector<Span>& second,
                   std::vector<Span>& out)
{
    size_t b = 0;
    for (const Span& span : first)
    {
        int32_t start = span.start;
        while (b < second.size() && second[b].end <= start)
        {
            ++b;
        }
        size_t c = b;
        while (c < second.size() && second[c].start < span.end)
        {
            if (second[c].start > start)
            {
                out.push_back({start, second[c].start});
            }
            start = std::max(start, second[c].end);
            ++c;
        }
        if (start < span.end)
        {
            out.push_back({start, span.end});
        }
    }
}

bool SameSpans(const std::vector<Rectangle>& rectangles, size_t first,
               const std::vector<Span>& spans)
{
    if (rectangles.size() - first != spans.size())
    {
        return false;
    }
    for (size_t index = 0; index < spans.size(); ++index)
    {
        const Rectangle& rectangle = rectangles[first + index];
        if (rectangle.x != spans[index].start || rectangle.x + rectangle.width != spans[index].end)
        {
            return false;
        }
    }
    return true;
}

} // namespace

Region::Region(const Rectangle& rectangle)
{
    const int32_t left = Clamp(rectangle.x);
    const int32_t top = Clamp(rectangle.y);
    const int32_t right = Clamp(static_cast<int64_t>(rectangle.x) + rectangle.width);
    const int32_t bottom = Clamp(static_cast<int64_t>(rectangle.y) + rectangle.height);
    if (rectangle.width > 0 && rectangle.height > 0 && left < right && top < bottom)
    {
        m_rectangles.push_back({left, top, right - left, bottom - top});
    }
}

void Region::Add(const Rectangle& rectangle)
{
    Add(Region(rectangle));
}

void Region::Add(const Region& other)
{
    Combine(other, Operation::Union);
}

void Region::Subtract(const Rectangle& rectangle)
{
    Subtract(Region(rectangle));
}

void Region::Subtract(const Region& other)
{
    Combine(other, Operation::Subtract);
}

void Region::Intersect(const Rectangle& rectangle)
{
    Intersect(Region(rectangle));
}

void Region::Intersect(const Region& other)
{
    Combine(other, Operation::Intersect);
}

void Region::Translate(int32_t dx, int32_t dy)
{
    std::vector<Rectangle> moved;
    moved.reserve(m_rectangles.size());
    for (const Rectangle& rectangle : m_rectangles)
    {
        const int32_t left = Clamp(static_cast<int64_t>(rectangle.x) + dx);
        const int32_t right = Clamp(static_cast<int64_t>(rectangle.x) + rectangle.width + dx);
        const int32_t top = Clamp(static_cast<int64_t>(rectangle.y) + dy);
        const int32_t bottom = Clamp(static_cast<int64_t>(rectangle.y) + rectangle.height + dy);
        if (left < right && top < bottom)
        {
            moved.push_back({left, top, right - left, bottom - top});
        }
    }
    m_rectangles.swap(moved);
}

void Region::Clear()
{
    m_rectangles.clear();
}

bool Region::Empty() const
{
    return m_rectangles.empty();
}

Rectangle Region::Bounds() const
{
    if (m_rectangles.empty())
    {
        return {0, 0, 0, 0};
    }
    int32_t left = m_rectangles.front().x;
    int32_t right = m_rectangles.front().x + m_rectangles.front().width;
    const int32_t top = m_rectangles.front().y;
    int32_t bottom = top;
    for (const Rectangle& rectangle : m_rectangles)
    {
        left = std::min(left, rectangle.x);
        right = std::max(right, rectangle.x + rectangle.width);
        bottom = std::max(bottom, rectangle.y + rectangle.height);
    }
    return {left, top, right - left, bottom - top};
}

bool Region::Contains(int32_t x, int32_t y) const
{
    for (const Rectangle& rectangle : m_rectangles)
    {
        if (rectangle.y > y)
        {
            break;
        }
        if (y < rectangle.y + rectangle.height && x >= rectangle.x &&
            x < rectangle.x + rectangle.width)
        {
            return true;
        }
    }
    return false;
}

size_t Region::RectangleCount() const
{
    return m_rectangles.size();
}

Rectangle Region::RectangleAt(size_t index) const
{
    return index < m_rectangles.size() ? m_rectangles[index] : Rectangle{0, 0, 0, 0};
}

bool Region::operator==(const Region& other) const
{
    if (m_rectangles.size() != other.m_rectangles.size())
    {
        return false;
    }
    for (size_t index = 0; index < m_rectangles.size(); ++index)
    {
        const Rectangle& left = m_rectangles[index];
        const Rectangle& right = other.m_rectangles[index];
        if (left.x != right.x || left.y != right.y || left.width != right.width ||
            left.height != right.height)
        {
            return false;
        }
    }
    return true;
}

// Works slab by slab between consecutive band edges and merges equal neighbours as it goes, so the
// result is canonical by construction.
void Region::Combine(const Region& other, Operation operation)
{
    const std::vector<Band> first_bands = BandsOf(m_rectangles);
    const std::vector<Band> second_bands = BandsOf(other.m_rectangles);

    std::vector<int32_t> edges;
    for (const Band& band : first_bands)
    {
        edges.push_back(band.top);
        edges.push_back(band.bottom);
    }
    for (const Band& band : second_bands)
    {
        edges.push_back(band.top);
        edges.push_back(band.bottom);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    std::vector<Rectangle> result;
    std::vector<Span> first_spans;
    std::vector<Span> second_spans;
    std::vector<Span> spans;
    size_t first_index = 0;
    size_t second_index = 0;
    size_t last_band_start = 0;
    for (size_t edge = 0; edge + 1 < edges.size(); ++edge)
    {
        const int32_t top = edges[edge];
        const int32_t bottom = edges[edge + 1];
        while (first_index < first_bands.size() && first_bands[first_index].bottom <= top)
        {
            ++first_index;
        }
        while (second_index < second_bands.size() && second_bands[second_index].bottom <= top)
        {
            ++second_index;
        }
        first_spans.clear();
        second_spans.clear();
        spans.clear();
        if (first_index < first_bands.size() && first_bands[first_index].top <= top)
        {
            SpansOf(m_rectangles, first_bands[first_index], first_spans);
        }
        if (second_index < second_bands.size() && second_bands[second_index].top <= top)
        {
            SpansOf(other.m_rectangles, second_bands[second_index], second_spans);
        }
        switch (operation)
        {
        case Operation::Union:
            UnionSpans(first_spans, second_spans, spans);
            break;
        case Operation::Subtract:
            SubtractSpans(first_spans, second_spans, spans);
            break;
        case Operation::Intersect:
            IntersectSpans(first_spans, second_spans, spans);
            break;
        }
        if (spans.empty())
        {
            continue;
        }
        const bool continues = last_band_start < result.size() &&
                               result[last_band_start].y + result[last_band_start].height == top &&
                               SameSpans(result, last_band_start, spans);
        if (continues)
        {
            for (size_t index = last_band_start; index < result.size(); ++index)
            {
                result[index].height = bottom - result[index].y;
            }
            continue;
        }
        last_band_start = result.size();
        for (const Span& span : spans)
        {
            result.push_back({span.start, top, span.end - span.start, bottom - top});
        }
    }
    m_rectangles.swap(result);
}

} // namespace FeatherOS::Graphics
