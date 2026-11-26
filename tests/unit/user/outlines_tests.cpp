#include <catch2/catch_test_macros.hpp>

#include "support/font_builder.h"
#include "support/outline_builder.h"
#include "user/graphics/canvas.h"
#include "user/graphics/outline_renderer.h"
#include "user/graphics/outlines.h"
#include "user/graphics/surface.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
namespace Support = FeatherOS::TestSupport;
using Graphics::OutlineItemView;
using Graphics::OutlinePaintKind;
using Graphics::OutlinePolylineView;
using Graphics::Outlines;
using Graphics::OutlineStatus;
using Support::Get16;
using Support::Get32;
using Support::Put16;
using Support::Put32;
using Support::TestDrawing;
using Support::TestItem;

namespace
{

using Bytes = std::vector<uint8_t>;
namespace O = Support::OutlineOffset;

std::vector<TestDrawing> SampleDrawings()
{
    TestDrawing box;
    box.name = "box";
    box.items.push_back(Support::Fill({Support::UnitPolygon({{4, 4}, {20, 4}, {20, 20}, {4, 20}})}));

    TestDrawing pointer;
    pointer.name = "pointer";
    pointer.view_width = 32;
    pointer.view_height = 48;
    pointer.hotspot = true;
    pointer.hotspot_x = 5 * 256;
    pointer.hotspot_y = 7 * 256 + 128;
    TestItem body = Support::Fill({Support::UnitPolygon({{0, 0}, {10, 20}, {5, 22}})}, 200);
    body.paint = 1;
    body.color = 0x80FF8040;
    pointer.items.push_back(body);
    TestItem outline = Support::Stroke(
        {Support::Polyline({{0, 0}, {2560, 5120}, {1280, 5632}}, true),
         Support::Polyline({{256, 256}, {512, 512}})},
        384);
    outline.paint = 2;
    outline.slot = 5;
    pointer.items.push_back(outline);

    TestDrawing zebra;
    zebra.name = "zebra";
    zebra.view_width = 16;
    zebra.view_height = 16;
    zebra.items.push_back(Support::Stroke({Support::Polyline({{0, 0}, {4096, 4096}})}, 512, 128));
    return {box, pointer, zebra};
}

Bytes Sample()
{
    return Support::BuildOutlines(SampleDrawings());
}

Bytes Sealed(const std::function<void(Bytes&)>& edit)
{
    Bytes bytes = Sample();
    edit(bytes);
    Support::SealOutlines(bytes);
    return bytes;
}

OutlineStatus Parse(const Bytes& bytes, Outlines* outlines = nullptr)
{
    Outlines local;
    return (outlines != nullptr ? outlines : &local)->LoadMemory(bytes.data(), bytes.size());
}

// The data of drawing 1 ("pointer"): its two items start here.
size_t PointerData(const Bytes& bytes)
{
    return Support::OutlineData(bytes, 1);
}

size_t PointerStroke(const Bytes& bytes)
{
    // 16 bytes of item, 4 of polyline header and three points.
    return PointerData(bytes) + O::ItemSize + O::PolylineHeader + 3 * 8;
}

} // namespace

TEST_CASE("A valid outlines file loads and gives back what was written", "[outlines]")
{
    const Bytes bytes = Sample();
    Outlines outlines;
    REQUIRE(Parse(bytes, &outlines) == OutlineStatus::Ok);
    REQUIRE(outlines.Valid());
    REQUIRE(outlines.Count() == 3);
    REQUIRE(outlines.Find("box") == 0);
    REQUIRE(outlines.Find("pointer") == 1);
    REQUIRE(outlines.Find("zebra") == 2);
    REQUIRE(std::string(outlines.Name(1)) == "pointer");
    REQUIRE(outlines.Name(3) == nullptr);
    REQUIRE(outlines.Name(-1) == nullptr);

    REQUIRE(outlines.ViewBox(0).width == 24);
    REQUIRE(outlines.ViewBox(1).width == 32);
    REQUIRE(outlines.ViewBox(1).height == 48);
    REQUIRE(outlines.ViewBox(3).width == 0);
    REQUIRE_FALSE(outlines.Hotspot(0).present);
    REQUIRE(outlines.Hotspot(1).present);
    REQUIRE(outlines.Hotspot(1).x == 5 * 256);
    REQUIRE(outlines.Hotspot(1).y == 7 * 256 + 128);
    REQUIRE_FALSE(outlines.Hotspot(9).present);
    REQUIRE(outlines.SinglePaint(0));
    REQUIRE_FALSE(outlines.SinglePaint(1));
    REQUIRE(outlines.SinglePaint(2));
    REQUIRE_FALSE(outlines.SinglePaint(7));
    REQUIRE(outlines.ItemCount(1) == 2);

    size_t cursor = 0;
    OutlineItemView item;
    REQUIRE(outlines.NextItem(1, &cursor, &item));
    REQUIRE_FALSE(item.stroke);
    REQUIRE(item.paint == OutlinePaintKind::Fixed);
    REQUIRE(item.color == 0x80FF8040U);
    REQUIRE(item.opacity == 200);
    REQUIRE(item.polyline_count == 1);
    OutlinePolylineView line;
    REQUIRE(item.NextPolyline(&line));
    REQUIRE(line.Count() == 3);
    REQUIRE(line.Closed());
    REQUIRE(line.At(1).x == 10 * 256);
    REQUIRE(line.At(1).y == 20 * 256);
    REQUIRE_FALSE(item.NextPolyline(&line));
    REQUIRE(outlines.NextItem(1, &cursor, &item));
    REQUIRE(item.stroke);
    REQUIRE(item.paint == OutlinePaintKind::Slot);
    REQUIRE(item.slot == 5);
    REQUIRE(item.width == 384);
    REQUIRE(item.polyline_count == 2);
    REQUIRE(item.NextPolyline(&line));
    REQUIRE(item.NextPolyline(&line));
    REQUIRE_FALSE(line.Closed());
    REQUIRE(line.Count() == 2);
    REQUIRE_FALSE(item.NextPolyline(&line));
    REQUIRE_FALSE(outlines.NextItem(1, &cursor, &item));
}

TEST_CASE("Find answers -1 for what the file lacks", "[outlines]")
{
    const Bytes bytes = Sample();
    Outlines outlines;
    REQUIRE(Outlines().Find("box") == -1);
    REQUIRE(Parse(bytes, &outlines) == OutlineStatus::Ok);
    REQUIRE(outlines.Find("boy") == -1);
    REQUIRE(outlines.Find("bo") == -1);
    REQUIRE(outlines.Find("") == -1);
    REQUIRE(outlines.Find(nullptr) == -1);
    REQUIRE(outlines.Find("a-name-that-is-far-too-long-for-a-drawing") == -1);
    REQUIRE(outlines.Find("zzz") == -1);
    REQUIRE(outlines.Find("a") == -1);
}

TEST_CASE("Every drawing of a large sorted table is found", "[outlines]")
{
    std::vector<TestDrawing> drawings;
    for (int index = 0; index < 200; ++index)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "d%03d", index);
        TestDrawing drawing;
        drawing.name = name;
        drawing.items.push_back(Support::Fill({Support::UnitPolygon({{0, 0}, {5, 0}, {0, 5}})}));
        drawings.push_back(drawing);
    }
    const Bytes bytes = Support::BuildOutlines(drawings);
    Outlines outlines;
    REQUIRE(Parse(bytes, &outlines) == OutlineStatus::Ok);
    for (int index = 0; index < 200; ++index)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "d%03d", index);
        REQUIRE(outlines.Find(name) == index);
    }
    REQUIRE(outlines.Find("d200") == -1);
}

TEST_CASE("The loader names its reason for refusing a file", "[outlines]")
{
    REQUIRE(Parse(Bytes(Outlines::MaximumFileSize + 1, 0)) == OutlineStatus::Size);
    REQUIRE(Parse(Bytes()) == OutlineStatus::Short);
    REQUIRE(Parse(Bytes(31, 0)) == OutlineStatus::Short);
    REQUIRE(Parse(Sealed([](Bytes& b) { b[0] = 'X'; })) == OutlineStatus::Magic);
    REQUIRE(Parse(Sealed([](Bytes& b) { Put16(b, O::Version, 2); })) == OutlineStatus::Version);
    REQUIRE(Parse(Sealed([](Bytes& b) { Put16(b, O::HeaderSize, 33); })) == OutlineStatus::Header);
    REQUIRE(Parse(Sealed([](Bytes& b) { b[O::Reserved + 3] = 1; })) == OutlineStatus::Header);
    REQUIRE(Parse(Sealed([](Bytes& b) { Put32(b, O::FileSize, Get32(b, O::FileSize) + 1); })) ==
            OutlineStatus::Length);
    REQUIRE(Parse(Sealed([](Bytes& b) { Put32(b, O::FileSize, Get32(b, O::FileSize) - 4); })) ==
            OutlineStatus::Length);
    Bytes bytes = Sample();
    bytes.push_back(0);
    REQUIRE(Parse(bytes) == OutlineStatus::Length);
    bytes = Sample();
    bytes.pop_back();
    REQUIRE(Parse(bytes) == OutlineStatus::Length);
}

TEST_CASE("A file whose checksum disagrees is refused", "[outlines]")
{
    Bytes bytes = Sample();
    bytes[bytes.size() - 5] ^= 0x01;
    REQUIRE(Parse(bytes) == OutlineStatus::Checksum);
    bytes = Sample();
    bytes[O::Checksum] ^= 0x80;
    REQUIRE(Parse(bytes) == OutlineStatus::Checksum);
    bytes = Sample();
    bytes[O::TableStart + O::ViewWidth] ^= 0x01;
    REQUIRE(Parse(bytes) == OutlineStatus::Checksum);
    for (size_t at = 0; at < bytes.size(); ++at)
    {
        Bytes damaged = Sample();
        damaged[at] ^= 0x10;
        REQUIRE(Parse(damaged) != OutlineStatus::Ok);
    }
}

TEST_CASE("A damaged table is refused with its own reason", "[outlines]")
{
    const size_t first = O::TableStart;
    const size_t second = O::TableStart + O::RecordSize;
    REQUIRE(Parse(Sealed([](Bytes& b) { Put32(b, O::Count, 0); })) == OutlineStatus::Layout);
    REQUIRE(Parse(Sealed([](Bytes& b) { Put32(b, O::Count, 0x7FFFFFFF); })) ==
            OutlineStatus::Layout);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, first + O::Flags, 2); })) ==
            OutlineStatus::Layout);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, first + O::HotspotX, 1); })) ==
            OutlineStatus::Layout);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, first + O::DataOffset, Get32(b, first + O::DataOffset) + 4); })) ==
            OutlineStatus::Layout);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, second + O::DataOffset, Get32(b, second + O::DataOffset) - 4); })) ==
            OutlineStatus::Layout);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, first + O::DataSize, 0x7FFFFFF0); })) ==
            OutlineStatus::Layout);
    // Bytes after the last drawing's data.
    Bytes tail = Sample();
    tail.insert(tail.end(), 8, 0);
    Put32(tail, O::FileSize, static_cast<uint32_t>(tail.size()));
    Support::SealOutlines(tail);
    REQUIRE(Parse(tail) == OutlineStatus::Layout);
    // A table that does not fit the file.
    Bytes cut(O::TableStart + 10, 0);
    cut[0] = 'F';
    cut[1] = 'V';
    cut[2] = 'O';
    cut[3] = 'L';
    Put16(cut, O::Version, 1);
    Put16(cut, O::HeaderSize, 32);
    Put32(cut, O::FileSize, static_cast<uint32_t>(cut.size()));
    Put32(cut, O::Count, 1);
    Support::SealOutlines(cut);
    REQUIRE(Parse(cut) == OutlineStatus::Layout);
}

TEST_CASE("Names must be sound and sorted", "[outlines]")
{
    const size_t first = O::TableStart;
    const size_t second = O::TableStart + O::RecordSize;
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[first] = 0; })) == OutlineStatus::Name);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[first] = ' '; })) == OutlineStatus::Name);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[first + 1] = 0x7F; })) == OutlineStatus::Name);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[first + 20] = 'x'; })) == OutlineStatus::Name);
    REQUIRE(Parse(Sealed([&](Bytes& b) {
                for (size_t at = 0; at < 32; ++at)
                {
                    b[first + at] = 'a';
                }
            })) == OutlineStatus::Name);
    // "box" then "pointer" swapped: unsorted.
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[first] = 'q'; })) == OutlineStatus::Order);
    // A duplicate name.
    REQUIRE(Parse(Sealed([&](Bytes& b) {
                for (size_t at = 0; at < 32; ++at)
                {
                    b[second + at] = b[first + at];
                }
            })) == OutlineStatus::Order);
    // A name that is a prefix of the next sorts first.
    TestDrawing a;
    a.name = "ab";
    a.items.push_back(Support::Fill({Support::UnitPolygon({{0, 0}, {1, 0}, {0, 1}})}));
    TestDrawing b = a;
    b.name = "abc";
    REQUIRE(Parse(Support::BuildOutlines({a, b})) == OutlineStatus::Ok);
    REQUIRE(Parse(Support::BuildOutlines({b, a})) == OutlineStatus::Order);
}

TEST_CASE("An item that runs past its drawing is refused", "[outlines]")
{
    const size_t first = O::TableStart;
    // The first drawing has one item of 16 + 4 + 4 * 8 bytes.
    REQUIRE(Get32(Sample(), first + O::DataSize) == 52);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, first + O::ItemCount, 2); })) ==
            OutlineStatus::Item);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, first + O::ItemCount, 0); })) ==
            OutlineStatus::Item);
    REQUIRE(Parse(Sealed([&](Bytes& b) {
                Put16(b, Support::OutlineData(b, 0) + O::Polylines, 2);
            })) == OutlineStatus::Item);
    REQUIRE(Parse(Sealed([&](Bytes& b) {
                Put16(b, Support::OutlineData(b, 0) + O::Polylines, 0);
            })) == OutlineStatus::Item);
    REQUIRE(Parse(Sealed([&](Bytes& b) {
                Put16(b, Support::OutlineData(b, 0) + O::ItemSize + O::PointCount, 5);
            })) == OutlineStatus::Item);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put16(b, Support::OutlineData(b, 0) + O::ItemPad, 1); })) ==
            OutlineStatus::Item);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[Support::OutlineData(b, 0) + O::Kind] = 2; })) ==
            OutlineStatus::Item);
    // A fill with a width.
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, Support::OutlineData(b, 0) + O::Width, 256); })) ==
            OutlineStatus::Item);
    // The point count claims more points than the drawing has bytes for.
    REQUIRE(Parse(Sealed([&](Bytes& b) {
                Put16(b, Support::OutlineData(b, 0) + O::ItemSize + O::PointCount, 0xFFFF);
            })) == OutlineStatus::Item);
    // The data size cut so that the last item is incomplete.
    Bytes shortened = Sample();
    Put32(shortened, first + O::DataSize, 48);
    Put32(shortened, first + O::RecordSize + O::DataOffset,
          Get32(shortened, first + O::RecordSize + O::DataOffset) - 4);
    Support::SealOutlines(shortened);
    REQUIRE(Parse(shortened) != OutlineStatus::Ok);
}

TEST_CASE("A polyline needs two points and a clean header", "[outlines]")
{
    const size_t polyline = Support::OutlineData(Sample(), 0) + O::ItemSize;
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put16(b, polyline + O::PointCount, 1); })) ==
            OutlineStatus::Polyline);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put16(b, polyline + O::PointCount, 0); })) ==
            OutlineStatus::Polyline);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[polyline + O::Closed] = 2; })) ==
            OutlineStatus::Polyline);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[polyline + O::PolylinePad] = 1; })) ==
            OutlineStatus::Polyline);
}

TEST_CASE("A paint or slot out of range is refused", "[outlines]")
{
    const Bytes sample = Sample();
    const size_t body = PointerData(sample);
    const size_t outline = PointerStroke(sample);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[body + O::Paint] = 3; })) == OutlineStatus::Paint);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[outline + O::Slot] = 8; })) == OutlineStatus::Paint);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[outline + O::Slot] = 255; })) == OutlineStatus::Paint);
    // A slot named by a paint that is not Slot, and a colour on one that is not Fixed.
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[body + O::Slot] = 1; })) == OutlineStatus::Paint);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, outline + O::Color, 1); })) ==
            OutlineStatus::Paint);
    REQUIRE(Parse(Sealed([&](Bytes& b) { b[outline + O::Slot] = 7; })) == OutlineStatus::Ok);
}

TEST_CASE("Points, widths, view boxes and hotspots stay within their ranges", "[outlines]")
{
    const size_t box_point = Support::OutlineData(Sample(), 0) + O::ItemSize + O::PolylineHeader;
    // The first drawing's view box is 24 units: x runs from -24 * 256 to 48 * 256.
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, box_point, static_cast<uint32_t>(-24 * 256)); })) ==
            OutlineStatus::Ok);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, box_point, static_cast<uint32_t>(-24 * 256 - 1)); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, box_point, 48 * 256); })) == OutlineStatus::Ok);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, box_point, 48 * 256 + 1); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, box_point + 4, 0x7FFFFFFF); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, box_point + 4, 0x80000000U); })) ==
            OutlineStatus::Range);

    const Bytes sample = Sample();
    const size_t outline = PointerStroke(sample);
    // The pointer's view box is 32 x 48: at most twice the smaller side, 64 units.
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, outline + O::Width, 64 * 256); })) ==
            OutlineStatus::Ok);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, outline + O::Width, 64 * 256 + 1); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, outline + O::Width, static_cast<uint32_t>(-1)); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, outline + O::Width, 0); })) == OutlineStatus::Ok);

    const size_t pointer = O::TableStart + O::RecordSize;
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, pointer + O::ViewWidth, 0); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, pointer + O::ViewHeight, 4097); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, pointer + O::HotspotX, 0x7FFFFFFF); })) ==
            OutlineStatus::Range);
    REQUIRE(Parse(Sealed([&](Bytes& b) { Put32(b, pointer + O::HotspotY, 0x80000000U); })) ==
            OutlineStatus::Range);
}

namespace
{

TestDrawing Spiky(const std::string& name, size_t points, size_t polylines = 1)
{
    TestDrawing drawing;
    drawing.name = name;
    Support::TestItem item;
    item.opacity = 255;
    for (size_t line = 0; line < polylines; ++line)
    {
        Support::TestPolyline polyline;
        const size_t share = line + 1 == polylines ? points - line * (points / polylines)
                                                   : points / polylines;
        for (size_t at = 0; at < share; ++at)
        {
            polyline.points.emplace_back(static_cast<int32_t>((at * 37) % 6000),
                                         (at % 2) != 0 ? 5900 : 100);
        }
        polyline.closed = true;
        item.polylines.push_back(polyline);
    }
    drawing.items.push_back(item);
    return drawing;
}

} // namespace

TEST_CASE("A drawing or a file with too many points is refused", "[outlines]")
{
    REQUIRE(Parse(Support::BuildOutlines({Spiky("a", Outlines::MaximumDrawingPoints)})) ==
            OutlineStatus::Ok);
    REQUIRE(Parse(Support::BuildOutlines({Spiky("a", Outlines::MaximumDrawingPoints + 1)})) ==
            OutlineStatus::Points);
    REQUIRE(Parse(Support::BuildOutlines({Spiky("a", Outlines::MaximumDrawingPoints + 1, 2)})) ==
            OutlineStatus::Points);
    REQUIRE(Parse(Support::BuildOutlines({Spiky("a", 60000)})) == OutlineStatus::Points);

    std::vector<TestDrawing> drawings;
    for (size_t index = 0; index < Outlines::MaximumFilePoints / Outlines::MaximumDrawingPoints;
         ++index)
    {
        drawings.push_back(Spiky(std::string(1, static_cast<char>('a' + index)),
                                 Outlines::MaximumDrawingPoints));
    }
    REQUIRE(Parse(Support::BuildOutlines(drawings)) == OutlineStatus::Ok);
    drawings.push_back(Spiky("z", 2));
    REQUIRE(Parse(Support::BuildOutlines(drawings)) == OutlineStatus::Points);
}

TEST_CASE("A file is loaded from disk and its read errors are refused", "[outlines]")
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "featheros_outlines_tests";
    std::filesystem::create_directories(directory);
    const std::string good = (directory / "good.fvo").string();
    const std::string grown = (directory / "grown.fvo").string();
    const std::string big = (directory / "big.fvo").string();
    const Bytes bytes = Sample();
    {
        std::ofstream(good, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        Bytes more = bytes;
        more.push_back(0);
        std::ofstream(grown, std::ios::binary)
            .write(reinterpret_cast<const char*>(more.data()),
                   static_cast<std::streamsize>(more.size()));
        const Bytes oversize(Outlines::MaximumFileSize + 1, 0);
        std::ofstream(big, std::ios::binary)
            .write(reinterpret_cast<const char*>(oversize.data()),
                   static_cast<std::streamsize>(oversize.size()));
    }
    Outlines outlines;
    REQUIRE(outlines.Load(good.c_str()) == OutlineStatus::Ok);
    REQUIRE(outlines.Find("pointer") == 1);
    REQUIRE(outlines.Load(grown.c_str()) == OutlineStatus::Length);
    REQUIRE_FALSE(outlines.Valid());
    REQUIRE(outlines.Load(big.c_str()) == OutlineStatus::Size);
    REQUIRE(outlines.Load((directory / "missing.fvo").string().c_str()) == OutlineStatus::Short);
    REQUIRE(outlines.Load(directory.string().c_str()) == OutlineStatus::Short);
    REQUIRE(outlines.Load(nullptr) == OutlineStatus::Short);

    REQUIRE(outlines.Load(good.c_str()) == OutlineStatus::Ok);
    Outlines moved(std::move(outlines));
    REQUIRE_FALSE(outlines.Valid());
    REQUIRE(moved.Find("box") == 0);
    Outlines assigned;
    assigned = std::move(moved);
    REQUIRE(assigned.Find("zebra") == 2);
    REQUIRE_FALSE(moved.Valid());
    std::filesystem::remove_all(directory);
}

TEST_CASE("Every status has a name", "[outlines]")
{
    for (int32_t status = 0; status <= static_cast<int32_t>(OutlineStatus::Points); ++status)
    {
        REQUIRE(std::string(Graphics::OutlineStatusName(static_cast<OutlineStatus>(status))) !=
                "unknown");
    }
    REQUIRE(std::string(Graphics::OutlineStatusName(static_cast<OutlineStatus>(99))) == "unknown");
}

namespace
{

// Reads everything a file that loaded can hold, and draws each drawing small, so a sanitizer sees
// every byte the renderer touches. The bytes sit in an allocation of exactly their size.
void UseEverything(const Bytes& bytes)
{
    auto exact = std::make_unique<uint8_t[]>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), exact.get());
    Outlines outlines;
    if (outlines.LoadMemory(exact.get(), bytes.size()) != OutlineStatus::Ok)
    {
        return;
    }
    Graphics::Surface surface(19, 13, Graphics::PixelFormat::PremultipliedArgb);
    surface.Fill(0);
    Graphics::OutlinePaints paints;
    for (uint32_t slot = 0; slot < Outlines::Slots; ++slot)
    {
        paints.slots[slot] = 0xFF000000U + slot * 0x101010U;
    }
    for (int32_t index = 0; index < outlines.Count(); ++index)
    {
        (void)outlines.Name(index);
        (void)outlines.Hotspot(index);
        size_t cursor = 0;
        OutlineItemView item;
        while (outlines.NextItem(index, &cursor, &item))
        {
            OutlinePolylineView line;
            while (item.NextPolyline(&line))
            {
                for (uint32_t point = 0; point < line.Count(); ++point)
                {
                    (void)line.At(point);
                }
            }
        }
        Graphics::Canvas(surface).DrawOutline(outlines, index, {-3, -2, 29, 21}, paints);
        Graphics::Canvas(surface).DrawOutline(outlines, index, {2, 1, 11, 9}, paints, {900});
        if (outlines.SinglePaint(index))
        {
            REQUIRE(Graphics::RenderOutlineMask(outlines, index, 7, 5).Valid());
        }
    }
}

} // namespace

TEST_CASE("2000 files with random bytes flipped are refused or render cleanly", "[outlines]")
{
    std::mt19937_64 random(0x0F1E2D3C4B5A6978ULL);
    const Bytes base = Sample();
    int accepted = 0;
    int refused = 0;
    std::vector<int> reasons(static_cast<size_t>(OutlineStatus::Range) + 1, 0);
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < 2000; ++round)
    {
        Bytes bytes = base;
        const int flips = 1 + static_cast<int>(random() % 4);
        for (int flip = 0; flip < flips; ++flip)
        {
            const size_t at = random() % bytes.size();
            switch (random() % 3)
            {
            case 0:
                bytes[at] ^= static_cast<uint8_t>(1U << (random() % 8));
                break;
            case 1:
                bytes[at] = static_cast<uint8_t>(random());
                break;
            default:
                bytes[at] = (random() % 2) != 0 ? 0xFF : 0x00;
                break;
            }
        }
        // Half of them get a checksum that agrees, so the structure checks see the damage.
        if ((round % 2) == 0)
        {
            Support::SealOutlines(bytes);
        }
        Outlines probe;
        const OutlineStatus status = probe.LoadMemory(bytes.data(), bytes.size());
        ++reasons[static_cast<size_t>(status)];
        (status == OutlineStatus::Ok ? accepted : refused) += 1;
        UseEverything(bytes);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    INFO("accepted " << accepted << ", refused " << refused << ", " << elapsed.count() << " s");
    REQUIRE(accepted > 0);
    REQUIRE(refused > 1000);
    // The structure checks, not only the checksum, were exercised.
    REQUIRE(reasons[static_cast<size_t>(OutlineStatus::Checksum)] > 500);
    REQUIRE(reasons[static_cast<size_t>(OutlineStatus::Range)] > 0);
    REQUIRE(reasons[static_cast<size_t>(OutlineStatus::Layout)] > 0);
}
