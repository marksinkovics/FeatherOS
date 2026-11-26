#ifndef FEATHEROS_USER_GRAPHICS_OUTLINES_H
#define FEATHEROS_USER_GRAPHICS_OUTLINES_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Graphics
{

// File layout, little-endian (the design's section 1.5):
//   header, 32 bytes: "FVOL", u16 version 1, u16 header size 32, u32 file size, u32 drawing
//     count, 8 reserved zero bytes, u64 FNV-1a checksum of the file with these 8 bytes as zero.
//   table, 64 bytes per drawing, sorted by name: char name[32] (1 to 31 characters from 0x21 to
//     0x7E, then zeros), u32 view box width and height in units (1 to 4096), u32 flags (bit 0:
//     the hotspot is present), i32 hotspot x and y, u32 data offset, u32 data size, u32 item
//     count. The data of drawing n starts where drawing n - 1's ends, the first right after the
//     table and the last at the end of the file.
//   item, 16 bytes: u8 kind (0 fill, 1 stroke), u8 paint (0 Current, 1 Fixed, 2 Slot), u8 slot
//     (0 to 7, 0 unless Slot), u8 opacity, u32 ARGB (0 unless Fixed), i32 stroke width (0 for a
//     fill), u16 polyline count (at least 1), u16 zero; then each polyline: u16 point count (at
//     least 2), u8 closed, u8 zero, then the points as i32 x, y.
// Points, widths and the hotspot are in 1/256 of a unit. A point lies on each axis between one view
// box before the view box and one after it, and a width is at most twice the smaller side. A
// drawing holds at most 8192 points and a file 65536.
enum class OutlineStatus : int32_t
{
    Ok = 0,
    Size,
    Short,
    Magic,
    Version,
    Header,
    Length,
    Checksum,
    Layout,
    Name,
    Order,
    Item,
    Polyline,
    Paint,
    Range,
    Points,
};

const char* OutlineStatusName(OutlineStatus status);

enum class OutlinePaintKind : uint8_t
{
    Current,
    Fixed,
    Slot,
};

struct OutlinePoint
{
    int32_t x;
    int32_t y;
};

// Reads from bytes the loader has checked; never owns them.
class OutlinePolylineView
{
public:
    OutlinePolylineView() = default;
    OutlinePolylineView(const uint8_t* points, uint32_t count, bool closed);

    uint32_t Count() const
    {
        return m_count;
    }
    bool Closed() const
    {
        return m_closed;
    }
    OutlinePoint At(uint32_t index) const;

private:
    const uint8_t* m_points = nullptr;
    uint32_t m_count = 0;
    bool m_closed = false;
};

class OutlineItemView
{
public:
    bool stroke = false;
    OutlinePaintKind paint = OutlinePaintKind::Current;
    uint8_t slot = 0;
    uint8_t opacity = 255;
    uint32_t color = 0;
    int32_t width = 0;
    uint32_t polyline_count = 0;

    // The next polyline of this item; false once there are none left.
    bool NextPolyline(OutlinePolylineView* polyline);

private:
    friend class Outlines;
    const uint8_t* m_position = nullptr;
    const uint8_t* m_end = nullptr;
    uint32_t m_left = 0;
};

struct OutlineSize
{
    int32_t width;
    int32_t height;
};

struct OutlineHotspot
{
    bool present;
    int32_t x;
    int32_t y;
};

class Outlines
{
public:
    static constexpr size_t MaximumFileSize = size_t{4} << 20;
    static constexpr size_t HeaderSize = 32;
    static constexpr size_t RecordSize = 64;
    static constexpr size_t ItemSize = 16;
    static constexpr size_t MaximumNameLength = 31;
    static constexpr int32_t MaximumViewBox = 4096;
    static constexpr uint32_t Slots = 8;
    // Points in one drawing, which the renderer keeps in memory, and in the whole file.
    static constexpr uint32_t MaximumDrawingPoints = 8192;
    static constexpr uint32_t MaximumFilePoints = 65536;

    Outlines();
    Outlines(const Outlines&) = delete;
    Outlines(Outlines&& other) noexcept;
    Outlines& operator=(const Outlines&) = delete;
    Outlines& operator=(Outlines&& other) noexcept;
    ~Outlines();

    // An unreadable file gives Short; a file over 4 MiB gives Size.
    OutlineStatus Load(const char* path);
    // Borrows the bytes, which must outlive this object.
    OutlineStatus LoadMemory(const uint8_t* bytes, size_t size);

    bool Valid() const;
    int32_t Count() const;
    // The file's checksum: two loaded files with different content differ in it.
    uint64_t Id() const;
    // -1 for a name the file lacks, one over 31 characters or no file loaded.
    int32_t Find(const char* name) const;
    const char* Name(int32_t index) const;
    // Zeros for an index out of range.
    OutlineSize ViewBox(int32_t index) const;
    OutlineHotspot Hotspot(int32_t index) const;
    // True when every item of the drawing paints with the caller's colour.
    bool SinglePaint(int32_t index) const;
    uint32_t ItemCount(int32_t index) const;
    // Items in file order: start with *cursor = 0.
    bool NextItem(int32_t index, size_t* cursor, OutlineItemView* item) const;

private:
    void Reset();
    const uint8_t* Record(int32_t index) const;

    uint8_t* m_owned;
    const uint8_t* m_bytes;
    size_t m_size;
    uint32_t m_count;
    uint64_t m_id;
};

} // namespace FeatherOS::Graphics

#endif
