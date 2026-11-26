#include "outline_cache.h"

#include <string.h>

namespace FeatherOS::Graphics
{

namespace
{

bool SamePaints(const OutlinePaints& a, const OutlinePaints& b)
{
    return a.current == b.current && memcmp(a.slots, b.slots, sizeof(a.slots)) == 0;
}

} // namespace

OutlineCache::Entry* OutlineCache::Find(uint64_t file, int32_t index, int32_t width, int32_t height,
                                        const OutlineOptions& options, bool image,
                                        const OutlinePaints& paints)
{
    for (const auto& entry : m_entries)
    {
        if (entry->file == file && entry->index == index && entry->width == width &&
            entry->height == height && entry->options == options && entry->image == image &&
            (!image || SamePaints(entry->paints, paints)))
        {
            entry->used = ++m_clock;
            return entry.get();
        }
    }
    return nullptr;
}

OutlineCache::Entry* OutlineCache::Keep(std::unique_ptr<Entry> entry)
{
    entry->used = ++m_clock;
    m_bytes += entry->bytes;
    Entry* kept = entry.get();
    m_entries.push_back(std::move(entry));
    while (m_bytes > Limit)
    {
        size_t oldest = 0;
        for (size_t at = 1; at < m_entries.size(); ++at)
        {
            if (m_entries[at]->used < m_entries[oldest]->used)
            {
                oldest = at;
            }
        }
        m_bytes -= m_entries[oldest]->bytes;
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    return kept;
}

const Mask* OutlineCache::GetMask(const Outlines& outlines, int32_t index, int32_t width,
                                  int32_t height, const OutlineOptions& options)
{
    const OutlinePaints none;
    if (int64_t{width} * height > static_cast<int64_t>(Limit))
    {
        return nullptr;
    }
    if (Entry* entry = Find(outlines.Id(), index, width, height, options, false, none))
    {
        return entry->mask.get();
    }
    Mask mask = RenderOutlineMask(outlines, index, width, height, options);
    if (!mask.Valid())
    {
        return nullptr;
    }
    ++m_renders;
    auto entry = std::make_unique<Entry>();
    entry->file = outlines.Id();
    entry->index = index;
    entry->width = width;
    entry->height = height;
    entry->options = options;
    entry->image = false;
    entry->bytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    entry->mask = std::make_unique<Mask>(std::move(mask));
    return Keep(std::move(entry))->mask.get();
}

const Surface* OutlineCache::GetImage(const Outlines& outlines, int32_t index, int32_t width,
                                      int32_t height, const OutlinePaints& paints,
                                      const OutlineOptions& options)
{
    if (Entry* entry = Find(outlines.Id(), index, width, height, options, true, paints))
    {
        return entry->surface.get();
    }
    if (index < 0 || index >= outlines.Count() ||
        int64_t{width} * height * int64_t{sizeof(uint32_t)} > static_cast<int64_t>(Limit))
    {
        return nullptr;
    }
    auto surface = std::make_unique<Surface>(width, height, PixelFormat::PremultipliedArgb);
    if (!surface->Valid())
    {
        return nullptr;
    }
    ++m_renders;
    surface->Fill(0);
    Canvas(*surface).DrawOutline(outlines, index, {0, 0, width, height}, paints, options);
    auto entry = std::make_unique<Entry>();
    entry->file = outlines.Id();
    entry->index = index;
    entry->width = width;
    entry->height = height;
    entry->options = options;
    entry->image = true;
    entry->paints = paints;
    entry->bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
    entry->surface = std::move(surface);
    return Keep(std::move(entry))->surface.get();
}

size_t OutlineCache::Bytes() const
{
    return m_bytes;
}

size_t OutlineCache::Entries() const
{
    return m_entries.size();
}

size_t OutlineCache::Renders() const
{
    return m_renders;
}

void OutlineCache::Clear()
{
    m_entries.clear();
    m_bytes = 0;
}

} // namespace FeatherOS::Graphics
