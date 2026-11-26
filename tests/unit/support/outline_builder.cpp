#include "support/outline_builder.h"

#include "support/font_builder.h"

#include <algorithm>
#include <cstring>

namespace FeatherOS::TestSupport
{

namespace
{

void Append16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void Append32(std::vector<uint8_t>& out, uint32_t value)
{
    Append16(out, static_cast<uint16_t>(value));
    Append16(out, static_cast<uint16_t>(value >> 16));
}

uint64_t Fnv1a(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        const bool inside = index >= OutlineOffset::Checksum && index < OutlineOffset::Checksum + 8;
        hash = (hash ^ (inside ? 0U : bytes[index])) * 0x100000001B3ULL;
    }
    return hash;
}

std::vector<uint8_t> ItemBytes(const TestItem& item)
{
    std::vector<uint8_t> out;
    out.push_back(item.stroke ? 1 : 0);
    out.push_back(item.paint);
    out.push_back(item.slot);
    out.push_back(item.opacity);
    Append32(out, item.color);
    Append32(out, static_cast<uint32_t>(item.width));
    Append16(out, static_cast<uint16_t>(item.polylines.size()));
    Append16(out, 0);
    for (const TestPolyline& line : item.polylines)
    {
        Append16(out, static_cast<uint16_t>(line.points.size()));
        out.push_back(line.closed ? 1 : 0);
        out.push_back(0);
        for (const auto& point : line.points)
        {
            Append32(out, static_cast<uint32_t>(point.first));
            Append32(out, static_cast<uint32_t>(point.second));
        }
    }
    return out;
}

} // namespace

std::vector<uint8_t> BuildOutlines(const std::vector<TestDrawing>& drawings)
{
    std::vector<std::vector<uint8_t>> data;
    for (const TestDrawing& drawing : drawings)
    {
        std::vector<uint8_t> bytes;
        for (const TestItem& item : drawing.items)
        {
            const std::vector<uint8_t> one = ItemBytes(item);
            bytes.insert(bytes.end(), one.begin(), one.end());
        }
        data.push_back(std::move(bytes));
    }
    std::vector<uint8_t> out;
    out.push_back('F');
    out.push_back('V');
    out.push_back('O');
    out.push_back('L');
    Append16(out, 1);
    Append16(out, 32);
    Append32(out, 0);
    Append32(out, static_cast<uint32_t>(drawings.size()));
    Append32(out, 0);
    Append32(out, 0);
    Append32(out, 0);
    Append32(out, 0);
    size_t offset = OutlineOffset::TableStart + drawings.size() * OutlineOffset::RecordSize;
    for (size_t index = 0; index < drawings.size(); ++index)
    {
        const TestDrawing& drawing = drawings[index];
        char name[32] = {};
        std::memcpy(name, drawing.name.data(), std::min<size_t>(drawing.name.size(), 32));
        out.insert(out.end(), name, name + 32);
        Append32(out, drawing.view_width);
        Append32(out, drawing.view_height);
        Append32(out, drawing.hotspot ? 1 : 0);
        Append32(out, static_cast<uint32_t>(drawing.hotspot_x));
        Append32(out, static_cast<uint32_t>(drawing.hotspot_y));
        Append32(out, static_cast<uint32_t>(offset));
        Append32(out, static_cast<uint32_t>(data[index].size()));
        Append32(out, static_cast<uint32_t>(drawing.items.size()));
        offset += data[index].size();
    }
    for (const auto& bytes : data)
    {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    Put32(out, OutlineOffset::FileSize, static_cast<uint32_t>(out.size()));
    SealOutlines(out);
    return out;
}

void SealOutlines(std::vector<uint8_t>& bytes)
{
    const uint64_t hash = Fnv1a(bytes);
    for (size_t index = 0; index < 8; ++index)
    {
        bytes[OutlineOffset::Checksum + index] = static_cast<uint8_t>(hash >> (8 * index));
    }
}

size_t OutlineRecord(size_t index)
{
    return OutlineOffset::TableStart + index * OutlineOffset::RecordSize;
}

size_t OutlineData(const std::vector<uint8_t>& bytes, size_t index)
{
    return Get32(bytes, OutlineRecord(index) + OutlineOffset::DataOffset);
}

TestPolyline Polyline(std::vector<std::pair<int32_t, int32_t>> points, bool closed)
{
    TestPolyline line;
    line.points = std::move(points);
    line.closed = closed;
    return line;
}

TestPolyline UnitPolygon(const std::vector<std::pair<int32_t, int32_t>>& corners)
{
    TestPolyline line;
    for (const auto& corner : corners)
    {
        line.points.emplace_back(corner.first * 256, corner.second * 256);
    }
    line.closed = true;
    return line;
}

TestItem Fill(std::vector<TestPolyline> polylines, uint8_t opacity)
{
    TestItem item;
    item.polylines = std::move(polylines);
    item.opacity = opacity;
    return item;
}

TestItem Stroke(std::vector<TestPolyline> polylines, int32_t width, uint8_t opacity)
{
    TestItem item;
    item.stroke = true;
    item.polylines = std::move(polylines);
    item.width = width;
    item.opacity = opacity;
    return item;
}

} // namespace FeatherOS::TestSupport
