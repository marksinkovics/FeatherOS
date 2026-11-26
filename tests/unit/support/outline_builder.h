#ifndef FEATHEROS_TESTS_SUPPORT_OUTLINE_BUILDER_H
#define FEATHEROS_TESTS_SUPPORT_OUTLINE_BUILDER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace FeatherOS::TestSupport
{

struct TestPolyline
{
    std::vector<std::pair<int32_t, int32_t>> points;
    bool closed = false;
};

struct TestItem
{
    bool stroke = false;
    // 0 Current, 1 Fixed, 2 Slot.
    uint8_t paint = 0;
    uint8_t slot = 0;
    uint8_t opacity = 255;
    uint32_t color = 0;
    int32_t width = 0;
    std::vector<TestPolyline> polylines;
};

struct TestDrawing
{
    std::string name;
    uint32_t view_width = 24;
    uint32_t view_height = 24;
    bool hotspot = false;
    int32_t hotspot_x = 0;
    int32_t hotspot_y = 0;
    std::vector<TestItem> items;
};

namespace OutlineOffset
{
constexpr size_t Magic = 0;
constexpr size_t Version = 4;
constexpr size_t HeaderSize = 6;
constexpr size_t FileSize = 8;
constexpr size_t Count = 12;
constexpr size_t Reserved = 16;
constexpr size_t Checksum = 24;
constexpr size_t TableStart = 32;
constexpr size_t RecordSize = 64;
// Inside a record.
constexpr size_t Name = 0;
constexpr size_t ViewWidth = 32;
constexpr size_t ViewHeight = 36;
constexpr size_t Flags = 40;
constexpr size_t HotspotX = 44;
constexpr size_t HotspotY = 48;
constexpr size_t DataOffset = 52;
constexpr size_t DataSize = 56;
constexpr size_t ItemCount = 60;
// Inside an item.
constexpr size_t Kind = 0;
constexpr size_t Paint = 1;
constexpr size_t Slot = 2;
constexpr size_t Opacity = 3;
constexpr size_t Color = 4;
constexpr size_t Width = 8;
constexpr size_t Polylines = 12;
constexpr size_t ItemPad = 14;
constexpr size_t ItemSize = 16;
// Inside a polyline.
constexpr size_t PointCount = 0;
constexpr size_t Closed = 2;
constexpr size_t PolylinePad = 3;
constexpr size_t PolylineHeader = 4;
} // namespace OutlineOffset

// Writes the drawings in the order given (so a test can break the order), then seals the file.
std::vector<uint8_t> BuildOutlines(const std::vector<TestDrawing>& drawings);

void SealOutlines(std::vector<uint8_t>& bytes);

size_t OutlineRecord(size_t index);
// The offset of the first byte of drawing `index`'s data.
size_t OutlineData(const std::vector<uint8_t>& bytes, size_t index);

TestPolyline Polyline(std::vector<std::pair<int32_t, int32_t>> points, bool closed = false);
// A rectangle, a triangle and so on, in units, wound the way the points are given.
TestPolyline UnitPolygon(const std::vector<std::pair<int32_t, int32_t>>& corners);
TestItem Fill(std::vector<TestPolyline> polylines, uint8_t opacity = 255);
TestItem Stroke(std::vector<TestPolyline> polylines, int32_t width, uint8_t opacity = 255);

} // namespace FeatherOS::TestSupport

#endif
