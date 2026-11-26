#ifndef FEATHEROS_USER_GRAPHICS_OUTLINE_CACHE_H
#define FEATHEROS_USER_GRAPHICS_OUTLINE_CACHE_H

#include "canvas.h"
#include "mask.h"
#include "outline_renderer.h"
#include "outlines.h"
#include "surface.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

namespace FeatherOS::Graphics
{

// Rendered drawings kept by name, size, paints and options, least recently used first out once
// they hold more than Limit bytes. Not thread-safe: one cache per thread. A returned pointer is
// valid until the next call on the cache. The key holds the file's checksum, not its address, so
// a replaced file never serves stale entries.
class OutlineCache
{
public:
    static constexpr size_t Limit = size_t{2} << 20;

    // Null for a drawing that is not single-paint, an index out of range, an invalid size or one
    // whose result would exceed Limit.
    const Mask* GetMask(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
                        const OutlineOptions& options = {});
    // A premultiplied surface; null for an index out of range, an invalid size or one whose
    // result would exceed Limit.
    const Surface* GetImage(const Outlines& outlines, int32_t index, int32_t width, int32_t height,
                            const OutlinePaints& paints, const OutlineOptions& options = {});

    size_t Bytes() const;
    size_t Entries() const;
    // Drawings rendered so far, which a hit does not raise.
    size_t Renders() const;
    void Clear();

private:
    struct Entry
    {
        uint64_t file;
        int32_t index;
        int32_t width;
        int32_t height;
        OutlineOptions options;
        bool image;
        OutlinePaints paints;
        uint64_t used;
        size_t bytes;
        std::unique_ptr<Mask> mask;
        std::unique_ptr<Surface> surface;
    };

    Entry* Find(uint64_t file, int32_t index, int32_t width, int32_t height,
                const OutlineOptions& options, bool image, const OutlinePaints& paints);
    Entry* Keep(std::unique_ptr<Entry> entry);

    std::vector<std::unique_ptr<Entry>> m_entries;
    size_t m_bytes = 0;
    uint64_t m_clock = 0;
    size_t m_renders = 0;
};

} // namespace FeatherOS::Graphics

#endif
