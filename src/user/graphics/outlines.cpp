#include "outlines.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FeatherOS::Graphics
{

namespace
{

constexpr uint16_t Version = 1;
constexpr size_t ChecksumOffset = 24;
constexpr size_t NameBytes = 32;
constexpr uint32_t FlagHotspot = 1;
constexpr uint8_t KindStroke = 1;
constexpr int64_t PointMargin = 256;

uint16_t Le16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t Le32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

int32_t LeSigned(const uint8_t* bytes)
{
    return static_cast<int32_t>(Le32(bytes));
}

uint64_t Le64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(Le32(bytes)) | (static_cast<uint64_t>(Le32(bytes + 4)) << 32);
}

uint64_t Checksum(const uint8_t* bytes, size_t size)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t index = 0; index < size; ++index)
    {
        const bool inside = index >= ChecksumOffset && index < ChecksumOffset + 8;
        hash = (hash ^ (inside ? 0U : bytes[index])) * 0x100000001B3ULL;
    }
    return hash;
}

struct Fields
{
    const uint8_t* name;
    int64_t view_width;
    int64_t view_height;
    uint32_t flags;
    int32_t hotspot_x;
    int32_t hotspot_y;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t item_count;
};

Fields ReadRecord(const uint8_t* record)
{
    return {record,
            Le32(record + 32),
            Le32(record + 36),
            Le32(record + 40),
            LeSigned(record + 44),
            LeSigned(record + 48),
            Le32(record + 52),
            Le32(record + 56),
            Le32(record + 60)};
}

bool InRange(int64_t value, int64_t extent)
{
    return value >= -extent * PointMargin && value <= 2 * extent * PointMargin;
}

// Checks one drawing's item data; `data` is exactly `record.data_size` bytes.
OutlineStatus CheckItems(const uint8_t* data, const Fields& record, uint32_t* file_points)
{
    size_t position = 0;
    uint32_t points = 0;
    const size_t size = record.data_size;
    const int64_t smaller =
        record.view_width < record.view_height ? record.view_width : record.view_height;
    const int64_t widest = 2 * PointMargin * smaller;
    if (record.item_count == 0)
    {
        return OutlineStatus::Item;
    }
    for (uint32_t item = 0; item < record.item_count; ++item)
    {
        if (size - position < Outlines::ItemSize)
        {
            return OutlineStatus::Item;
        }
        const uint8_t* head = data + position;
        position += Outlines::ItemSize;
        if (head[0] > KindStroke)
        {
            return OutlineStatus::Item;
        }
        const uint8_t paint = head[1];
        if (paint > static_cast<uint8_t>(OutlinePaintKind::Slot) || head[2] >= Outlines::Slots ||
            (paint != static_cast<uint8_t>(OutlinePaintKind::Slot) && head[2] != 0) ||
            (paint != static_cast<uint8_t>(OutlinePaintKind::Fixed) && Le32(head + 4) != 0))
        {
            return OutlineStatus::Paint;
        }
        const int32_t width = LeSigned(head + 8);
        if (head[0] == KindStroke ? (width < 0 || width > widest) : width != 0)
        {
            return head[0] == KindStroke ? OutlineStatus::Range : OutlineStatus::Item;
        }
        const uint32_t polylines = Le16(head + 12);
        if (polylines == 0 || Le16(head + 14) != 0)
        {
            return OutlineStatus::Item;
        }
        for (uint32_t polyline = 0; polyline < polylines; ++polyline)
        {
            if (size - position < 4)
            {
                return OutlineStatus::Item;
            }
            const uint8_t* start = data + position;
            const uint32_t count = Le16(start);
            position += 4;
            if (count < 2 || start[2] > 1 || start[3] != 0)
            {
                return OutlineStatus::Polyline;
            }
            if ((size - position) / 8 < count)
            {
                return OutlineStatus::Item;
            }
            points += count;
            *file_points += count;
            if (points > Outlines::MaximumDrawingPoints ||
                *file_points > Outlines::MaximumFilePoints)
            {
                return OutlineStatus::Points;
            }
            for (uint32_t point = 0; point < count; ++point)
            {
                const uint8_t* at = data + position + static_cast<size_t>(point) * 8;
                if (!InRange(LeSigned(at), record.view_width) ||
                    !InRange(LeSigned(at + 4), record.view_height))
                {
                    return OutlineStatus::Range;
                }
            }
            position += static_cast<size_t>(count) * 8;
        }
    }
    return position == size ? OutlineStatus::Ok : OutlineStatus::Item;
}

OutlineStatus CheckName(const uint8_t* name)
{
    size_t length = 0;
    while (length < NameBytes && name[length] != 0)
    {
        if (name[length] < 0x21 || name[length] > 0x7E)
        {
            return OutlineStatus::Name;
        }
        ++length;
    }
    if (length == 0 || length > Outlines::MaximumNameLength)
    {
        return OutlineStatus::Name;
    }
    for (size_t index = length; index < NameBytes; ++index)
    {
        if (name[index] != 0)
        {
            return OutlineStatus::Name;
        }
    }
    return OutlineStatus::Ok;
}

OutlineStatus CheckRecord(const uint8_t* bytes, size_t size, const Fields& record, size_t expected,
                          uint32_t* file_points)
{
    if (record.view_width < 1 || record.view_width > Outlines::MaximumViewBox ||
        record.view_height < 1 || record.view_height > Outlines::MaximumViewBox)
    {
        return OutlineStatus::Range;
    }
    const bool hotspot = (record.flags & FlagHotspot) != 0;
    if ((record.flags & ~FlagHotspot) != 0 ||
        (!hotspot && (record.hotspot_x != 0 || record.hotspot_y != 0)) ||
        record.data_offset != expected || record.data_size > size - expected)
    {
        return OutlineStatus::Layout;
    }
    if (hotspot && (!InRange(record.hotspot_x, record.view_width) ||
                    !InRange(record.hotspot_y, record.view_height)))
    {
        return OutlineStatus::Range;
    }
    return CheckItems(bytes + record.data_offset, record, file_points);
}

OutlineStatus Check(const uint8_t* bytes, size_t size, uint32_t* count)
{
    if (size > Outlines::MaximumFileSize)
    {
        return OutlineStatus::Size;
    }
    if (size < Outlines::HeaderSize)
    {
        return OutlineStatus::Short;
    }
    if (bytes[0] != 'F' || bytes[1] != 'V' || bytes[2] != 'O' || bytes[3] != 'L')
    {
        return OutlineStatus::Magic;
    }
    if (Le16(bytes + 4) != Version)
    {
        return OutlineStatus::Version;
    }
    if (Le16(bytes + 6) != Outlines::HeaderSize || Le64(bytes + 16) != 0)
    {
        return OutlineStatus::Header;
    }
    if (Le32(bytes + 8) != size)
    {
        return OutlineStatus::Length;
    }
    if (Le64(bytes + ChecksumOffset) != Checksum(bytes, size))
    {
        return OutlineStatus::Checksum;
    }
    const uint32_t drawings = Le32(bytes + 12);
    if (drawings == 0 ||
        (size - Outlines::HeaderSize) / Outlines::RecordSize < static_cast<size_t>(drawings))
    {
        return OutlineStatus::Layout;
    }
    size_t expected = Outlines::HeaderSize + static_cast<size_t>(drawings) * Outlines::RecordSize;
    const uint8_t* previous = nullptr;
    uint32_t file_points = 0;
    for (uint32_t index = 0; index < drawings; ++index)
    {
        const Fields record =
            ReadRecord(bytes + Outlines::HeaderSize + index * Outlines::RecordSize);
        OutlineStatus status = CheckName(record.name);
        if (status != OutlineStatus::Ok)
        {
            return status;
        }
        if (previous != nullptr && memcmp(previous, record.name, NameBytes) >= 0)
        {
            return OutlineStatus::Order;
        }
        previous = record.name;
        status = CheckRecord(bytes, size, record, expected, &file_points);
        if (status != OutlineStatus::Ok)
        {
            return status;
        }
        expected += record.data_size;
    }
    if (expected != size)
    {
        return OutlineStatus::Layout;
    }
    *count = drawings;
    return OutlineStatus::Ok;
}

} // namespace

const char* OutlineStatusName(OutlineStatus status)
{
    switch (status)
    {
    case OutlineStatus::Ok:
        return "ok";
    case OutlineStatus::Size:
        return "size";
    case OutlineStatus::Short:
        return "short";
    case OutlineStatus::Magic:
        return "magic";
    case OutlineStatus::Version:
        return "version";
    case OutlineStatus::Header:
        return "header";
    case OutlineStatus::Length:
        return "length";
    case OutlineStatus::Checksum:
        return "checksum";
    case OutlineStatus::Layout:
        return "layout";
    case OutlineStatus::Name:
        return "name";
    case OutlineStatus::Order:
        return "order";
    case OutlineStatus::Item:
        return "item";
    case OutlineStatus::Polyline:
        return "polyline";
    case OutlineStatus::Paint:
        return "paint";
    case OutlineStatus::Range:
        return "range";
    case OutlineStatus::Points:
        return "points";
    }
    return "unknown";
}

OutlinePolylineView::OutlinePolylineView(const uint8_t* points, uint32_t count, bool closed)
    : m_points(points), m_count(count), m_closed(closed)
{
}

OutlinePoint OutlinePolylineView::At(uint32_t index) const
{
    if (index >= m_count)
    {
        return {0, 0};
    }
    const uint8_t* at = m_points + static_cast<size_t>(index) * 8;
    return {LeSigned(at), LeSigned(at + 4)};
}

bool OutlineItemView::NextPolyline(OutlinePolylineView* polyline)
{
    if (m_left == 0 || m_end - m_position < 4)
    {
        m_left = 0;
        return false;
    }
    const uint32_t count = Le16(m_position);
    const bool closed = m_position[2] != 0;
    const uint8_t* points = m_position + 4;
    if (static_cast<size_t>(m_end - points) / 8 < count)
    {
        m_left = 0;
        return false;
    }
    *polyline = OutlinePolylineView(points, count, closed);
    m_position = points + static_cast<size_t>(count) * 8;
    --m_left;
    return true;
}

Outlines::Outlines() : m_owned(nullptr), m_bytes(nullptr), m_size(0), m_count(0), m_id(0) {}

Outlines::Outlines(Outlines&& other) noexcept
    : m_owned(other.m_owned), m_bytes(other.m_bytes), m_size(other.m_size), m_count(other.m_count),
      m_id(other.m_id)
{
    other.m_owned = nullptr;
    other.Reset();
}

Outlines& Outlines::operator=(Outlines&& other) noexcept
{
    if (this != &other)
    {
        free(m_owned);
        m_owned = other.m_owned;
        m_bytes = other.m_bytes;
        m_size = other.m_size;
        m_count = other.m_count;
        m_id = other.m_id;
        other.m_owned = nullptr;
        other.Reset();
    }
    return *this;
}

Outlines::~Outlines()
{
    free(m_owned);
}

void Outlines::Reset()
{
    free(m_owned);
    m_owned = nullptr;
    m_bytes = nullptr;
    m_size = 0;
    m_count = 0;
    m_id = 0;
}

OutlineStatus Outlines::LoadMemory(const uint8_t* bytes, size_t size)
{
    Reset();
    uint32_t count = 0;
    const OutlineStatus status = Check(bytes, size, &count);
    if (status != OutlineStatus::Ok)
    {
        return status;
    }
    m_bytes = bytes;
    m_size = size;
    m_count = count;
    m_id = Le64(bytes + ChecksumOffset);
    return status;
}

OutlineStatus Outlines::Load(const char* path)
{
    Reset();
    if (path == nullptr)
    {
        return OutlineStatus::Short;
    }
    const int descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
    {
        return OutlineStatus::Short;
    }
    struct stat information;
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0)
    {
        close(descriptor);
        return OutlineStatus::Short;
    }
    if (static_cast<uint64_t>(information.st_size) > MaximumFileSize)
    {
        close(descriptor);
        return OutlineStatus::Size;
    }
    // One byte more than the file had, so a file that grew since fstat reaches the checks with a
    // size that disagrees with its header.
    const size_t capacity = static_cast<size_t>(information.st_size) + 1;
    auto* buffer = static_cast<uint8_t*>(malloc(capacity));
    size_t size = 0;
    bool failed = buffer == nullptr;
    while (!failed && size < capacity)
    {
        const ssize_t count = read(descriptor, buffer + size, capacity - size);
        if (count < 0)
        {
            failed = true;
        }
        else if (count == 0)
        {
            break;
        }
        else
        {
            size += static_cast<size_t>(count);
        }
    }
    close(descriptor);
    if (failed)
    {
        free(buffer);
        return OutlineStatus::Short;
    }
    const OutlineStatus status = LoadMemory(buffer, size);
    if (status != OutlineStatus::Ok)
    {
        free(buffer);
        return status;
    }
    m_owned = buffer;
    return status;
}

bool Outlines::Valid() const
{
    return m_count != 0;
}

int32_t Outlines::Count() const
{
    return static_cast<int32_t>(m_count);
}

uint64_t Outlines::Id() const
{
    return m_id;
}

const uint8_t* Outlines::Record(int32_t index) const
{
    if (index < 0 || static_cast<uint32_t>(index) >= m_count)
    {
        return nullptr;
    }
    return m_bytes + HeaderSize + static_cast<size_t>(index) * RecordSize;
}

int32_t Outlines::Find(const char* name) const
{
    if (m_count == 0 || name == nullptr || strlen(name) > MaximumNameLength)
    {
        return -1;
    }
    uint8_t wanted[NameBytes] = {};
    for (size_t at = 0; name[at] != 0; ++at)
    {
        wanted[at] = static_cast<uint8_t>(name[at]);
    }
    uint32_t low = 0;
    uint32_t high = m_count;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2;
        const int order = memcmp(Record(static_cast<int32_t>(middle)), wanted, NameBytes);
        if (order == 0)
        {
            return static_cast<int32_t>(middle);
        }
        if (order < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return -1;
}

const char* Outlines::Name(int32_t index) const
{
    const uint8_t* record = Record(index);
    return record == nullptr ? nullptr : reinterpret_cast<const char*>(record);
}

OutlineSize Outlines::ViewBox(int32_t index) const
{
    const uint8_t* record = Record(index);
    if (record == nullptr)
    {
        return {0, 0};
    }
    return {static_cast<int32_t>(Le32(record + 32)), static_cast<int32_t>(Le32(record + 36))};
}

OutlineHotspot Outlines::Hotspot(int32_t index) const
{
    const uint8_t* record = Record(index);
    if (record == nullptr || (Le32(record + 40) & FlagHotspot) == 0)
    {
        return {false, 0, 0};
    }
    return {true, LeSigned(record + 44), LeSigned(record + 48)};
}

uint32_t Outlines::ItemCount(int32_t index) const
{
    const uint8_t* record = Record(index);
    return record == nullptr ? 0 : Le32(record + 60);
}

bool Outlines::SinglePaint(int32_t index) const
{
    size_t cursor = 0;
    OutlineItemView item;
    bool any = false;
    while (NextItem(index, &cursor, &item))
    {
        any = true;
        if (item.paint != OutlinePaintKind::Current)
        {
            return false;
        }
    }
    return any;
}

bool Outlines::NextItem(int32_t index, size_t* cursor, OutlineItemView* item) const
{
    const uint8_t* record = Record(index);
    if (record == nullptr)
    {
        return false;
    }
    const Fields fields = ReadRecord(record);
    const size_t size = fields.data_size;
    const size_t position = *cursor;
    if (position >= size || size - position < ItemSize)
    {
        return false;
    }
    const uint8_t* data = m_bytes + fields.data_offset;
    const uint8_t* head = data + position;
    item->stroke = head[0] == KindStroke;
    item->paint = static_cast<OutlinePaintKind>(head[1]);
    item->slot = head[2];
    item->opacity = head[3];
    item->color = Le32(head + 4);
    item->width = LeSigned(head + 8);
    item->polyline_count = Le16(head + 12);
    item->m_position = head + ItemSize;
    item->m_end = data + size;
    item->m_left = item->polyline_count;
    const uint8_t* walk = item->m_position;
    for (uint32_t polyline = 0; polyline < item->polyline_count; ++polyline)
    {
        if (item->m_end - walk < 4)
        {
            return false;
        }
        const uint32_t count = Le16(walk);
        walk += 4;
        if (static_cast<size_t>(item->m_end - walk) / 8 < count)
        {
            return false;
        }
        walk += static_cast<size_t>(count) * 8;
    }
    *cursor = static_cast<size_t>(walk - data);
    return true;
}

} // namespace FeatherOS::Graphics
