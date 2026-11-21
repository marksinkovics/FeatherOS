#include <catch2/catch_test_macros.hpp>

#include "kernel/framebuffer/console_font.h"
#include "support/font_builder.h"
#include "user/text/font_data.h"
#include "user/text/utf8.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Text = FeatherOS::Text;
namespace Support = FeatherOS::TestSupport;
using Support::BitmapOffset;
using Support::Get16;
using Support::Get32;
using Support::Put16;
using Support::Put32;
using Support::RecordOffset;
using Support::FontOffset::Ascent;
using Support::FontOffset::BitmapsOffset;
using Support::FontOffset::BitmapsSize;
using Support::FontOffset::CellWidth;
using Support::FontOffset::CoverageBits;
using Support::FontOffset::FileSize;
using Support::FontOffset::Flags;
using Support::FontOffset::GlyphCount;
using Support::FontOffset::HeaderSize;
using Support::FontOffset::Reserved;
using Support::FontOffset::TableOffset;
using Support::FontOffset::TableSize;
using Support::FontOffset::TextOffset;
using Support::FontOffset::TextSize;
using Support::FontOffset::Version;
using Text::FontStatus;
using Text::FontView;

namespace
{

using Bytes = std::vector<uint8_t>;

FontStatus Parse(const Bytes& bytes, FontView* view = nullptr)
{
    FontView local;
    return FontView::Parse(bytes.data(), bytes.size(), view != nullptr ? view : &local);
}

Bytes Sealed(const std::function<void(Bytes&)>& edit)
{
    Bytes bytes = Support::BuildFont(Support::SampleFont());
    edit(bytes);
    Support::Seal(bytes);
    return bytes;
}

Bytes Unsealed(const std::function<void(Bytes&)>& edit)
{
    Bytes bytes = Support::BuildFont(Support::SampleFont());
    edit(bytes);
    return bytes;
}

size_t IndexOf(uint32_t code_point)
{
    const Support::TestFont font = Support::SampleFont();
    for (size_t index = 0; index < font.glyphs.size(); ++index)
    {
        if (font.glyphs[index].code_point == code_point)
        {
            return index;
        }
    }
    FAIL("code point " << code_point << " is not in the sample font");
    return 0;
}

Bytes ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string FontPath(const std::string& name)
{
    return std::string(FEATHEROS_FONT_DIRECTORY) + "/" + name;
}

} // namespace

TEST_CASE("a well-formed font parses and reports its header", "[font]")
{
    const Bytes bytes = Support::BuildFont(Support::SampleFont());
    FontView view;
    REQUIRE(Parse(bytes, &view) == FontStatus::Ok);
    CHECK(view.Valid());
    CHECK_FALSE(view.Monospace());
    CHECK(view.EmSize() == 13);
    CHECK(view.LineHeight() == 16);
    CHECK(view.Ascent() == 13);
    CHECK(view.Descent() == 3);
    CHECK(view.CellWidth() == 0);
    CHECK(view.GlyphCount() == 1 + 95 + 5);
    CHECK(std::string(view.Name()) == "test 16");
    CHECK(std::string(view.Notice()) == "Made for the tests. SIL Open Font License 1.1.");
    CHECK_FALSE(FontView().Valid());
}

TEST_CASE("zero padding after the text is accepted", "[font]")
{
    Support::TestFont font = Support::SampleFont();
    font.text_suffix = std::string(3, '\0');
    CHECK(Parse(Support::BuildFont(font)) == FontStatus::Ok);
    font.text_suffix = "x";
    CHECK(Parse(Support::BuildFont(font)) == FontStatus::Text);
}

TEST_CASE("a monospace font parses when every advance is the cell width", "[font]")
{
    Support::TestFont font = Support::SampleFont();
    font.monospace = true;
    font.cell_width = 7;
    for (Support::TestGlyph& glyph : font.glyphs)
    {
        glyph.advance = 7;
    }
    FontView view;
    REQUIRE(Parse(Support::BuildFont(font), &view) == FontStatus::Ok);
    CHECK(view.Monospace());
    CHECK(view.CellWidth() == 7);
}

TEST_CASE("lookup finds records at both ends, in the dense run and by search", "[font]")
{
    const Bytes bytes = Support::BuildFont(Support::SampleFont());
    FontView view;
    REQUIRE(Parse(bytes, &view) == FontStatus::Ok);

    CHECK(view.Find(0).code_point == 0);
    CHECK(view.Find(0x20).code_point == 0x20);
    CHECK(view.Find('A').code_point == 'A');
    CHECK(view.Find(0x7E).code_point == 0x7E);
    CHECK(view.Find(0xA0).code_point == 0xA0);
    CHECK(view.Find(0xE9).code_point == 0xE9);
    CHECK(view.Find(0x391).code_point == 0x391);
    CHECK(view.Find(0x2500).code_point == 0x2500);
    CHECK(view.Find(0x10FFFF).code_point == 0x10FFFF);
    for (char32_t code_point = 0x20; code_point <= 0x7E; ++code_point)
    {
        CHECK(view.Find(code_point).code_point == code_point);
        CHECK(view.Contains(code_point));
    }

    CHECK_FALSE(view.Contains(0x1F));
    CHECK_FALSE(view.Contains(0x7F));
    CHECK_FALSE(view.Contains(0xA1));
    CHECK_FALSE(view.Contains(0x2501));
    CHECK_FALSE(view.Contains(0x110000));
    CHECK(view.Find(0x7F).code_point == 0);
    CHECK(view.Find(0xA1).code_point == 0);
    CHECK(view.Find(0x10FFFE).code_point == 0);
    CHECK(view.Find(0xFFFFFFFF).code_point == 0);
    CHECK(view.Find(0x7F).advance == 7);
}

TEST_CASE("lookup works without a dense ASCII run", "[font]")
{
    Support::TestFont font = Support::SampleFont();
    font.glyphs.erase(font.glyphs.begin() + 1 + 10);
    const Bytes bytes = Support::BuildFont(font);
    FontView view;
    REQUIRE(Parse(bytes, &view) == FontStatus::Ok);
    CHECK(view.Find('A').code_point == 'A');
    CHECK(view.Find(0x20 + 10).code_point == 0);
    CHECK(view.Find(0x7E).code_point == 0x7E);
    CHECK(view.Find(0x391).code_point == 0x391);
}

TEST_CASE("coverage reads the packed nibbles of a glyph written in the test", "[font]")
{
    const Bytes bytes = Support::BuildFont(Support::SampleFont());
    const size_t offset = BitmapOffset(bytes, IndexOf(0xA0));
    CHECK(bytes[offset + 0] == 0x12);
    CHECK(bytes[offset + 1] == 0x30);
    CHECK(bytes[offset + 2] == 0xF0);
    CHECK(bytes[offset + 3] == 0x70);

    FontView view;
    REQUIRE(Parse(bytes, &view) == FontStatus::Ok);
    const Text::Glyph glyph = view.Find(0xA0);
    CHECK(glyph.width == 3);
    CHECK(glyph.height == 2);
    CHECK(glyph.left == -1);
    CHECK(glyph.top == 2);
    CHECK(glyph.advance == 5);
    const uint8_t expected[2][3] = {{1, 2, 3}, {15, 0, 7}};
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            CHECK(view.Coverage(glyph, x, y) == expected[y][x]);
        }
    }
    CHECK(view.Coverage(glyph, 3, 0) == 0);
    CHECK(view.Coverage(glyph, 0, 2) == 0);
    CHECK(view.Coverage(glyph, -1, 0) == 0);
    CHECK(view.Coverage(glyph, 0, -1) == 0);
}

TEST_CASE("coverage of every sample glyph matches what was written", "[font]")
{
    const Support::TestFont font = Support::SampleFont();
    FontView view;
    const Bytes bytes = Support::BuildFont(font);
    REQUIRE(Parse(bytes, &view) == FontStatus::Ok);
    for (const Support::TestGlyph& written : font.glyphs)
    {
        const Text::Glyph glyph = view.Find(written.code_point);
        REQUIRE(glyph.code_point == written.code_point);
        REQUIRE(glyph.width == written.width);
        REQUIRE(glyph.height == written.height);
        for (int y = 0; y < glyph.height; ++y)
        {
            for (int x = 0; x < glyph.width; ++x)
            {
                REQUIRE(view.Coverage(glyph, x, y) ==
                        written.coverage[static_cast<size_t>(y * glyph.width + x)]);
            }
        }
    }
}

TEST_CASE("a file over 4 MiB or under the header is refused first", "[font]")
{
    const Bytes bytes = Support::BuildFont(Support::SampleFont());
    FontView view;
    CHECK(FontView::Parse(bytes.data(), 4U * 1024U * 1024U + 1, &view) == FontStatus::Size);
    CHECK(FontView::Parse(bytes.data(), 63, &view) == FontStatus::Short);
    CHECK(FontView::Parse(bytes.data(), 0, &view) == FontStatus::Short);
    CHECK_FALSE(view.Valid());
}

namespace
{

struct Refusal
{
    const char* name;
    FontStatus expected;
    std::function<Bytes()> make;
};

} // namespace

TEST_CASE("every refusal of the format is reported", "[font]")
{
    const size_t last = Support::SampleFont().glyphs.size() - 1;
    const size_t empty = IndexOf(0x20);
    const std::vector<Refusal> cases = {
        {"magic", FontStatus::Magic, [] { return Sealed([](Bytes& b) { b[0] = 'X'; }); }},
        {"version", FontStatus::Version,
         [] { return Sealed([](Bytes& b) { Put16(b, Version, 2); }); }},
        {"header size", FontStatus::Header,
         [] { return Sealed([](Bytes& b) { Put16(b, HeaderSize, 65); }); }},
        {"coverage bits", FontStatus::Header,
         [] { return Sealed([](Bytes& b) { b[CoverageBits] = 8; }); }},
        {"unknown flag", FontStatus::Header,
         [] { return Sealed([](Bytes& b) { Put32(b, Flags, 2); }); }},
        {"reserved byte", FontStatus::Reserved,
         [] { return Sealed([](Bytes& b) { b[Reserved] = 1; }); }},
        {"record reserved", FontStatus::Reserved,
         [=] { return Sealed([=](Bytes& b) { Put16(b, RecordOffset(b, 5) + 18, 1); }); }},
        {"padding nibble", FontStatus::Reserved,
         []
         {
             return Sealed(
                 [](Bytes& b)
                 {
                     const size_t index = IndexOf('A');
                     const uint16_t width = Get16(b, RecordOffset(b, index) + 8);
                     REQUIRE(width % 2 == 1);
                     b[BitmapOffset(b, index) + (width + 1) / 2 - 1] |= 0x01;
                 });
         }},
        {"length larger", FontStatus::Length,
         [] { return Sealed([](Bytes& b) { Put32(b, FileSize, Get32(b, FileSize) + 1); }); }},
        {"length smaller", FontStatus::Length,
         [] { return Sealed([](Bytes& b) { Put32(b, FileSize, Get32(b, FileSize) - 1); }); }},
        {"appended byte", FontStatus::Length,
         [] { return Sealed([](Bytes& b) { b.push_back(0); }); }},
        {"checksum field", FontStatus::Checksum,
         [] { return Unsealed([](Bytes& b) { b[Support::FontOffset::Checksum] ^= 1; }); }},
        {"checksum header", FontStatus::Checksum,
         [] { return Unsealed([](Bytes& b) { Put16(b, Support::FontOffset::Em, 14); }); }},
        {"table unaligned", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TableOffset, 66); }); }},
        {"bitmaps unaligned", FontStatus::Layout,
         []
         {
             return Sealed([](Bytes& b) { Put32(b, BitmapsOffset, Get32(b, BitmapsOffset) + 1); });
         }},
        {"text unaligned", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TextOffset, Get32(b, TextOffset) + 1); }); }},
        {"table inside the header", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TableOffset, 60); }); }},
        {"table size", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TableSize, Get32(b, TableSize) - 20); }); }},
        {"glyph count", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, GlyphCount, Get32(b, GlyphCount) + 1); }); }},
        {"table over bitmaps", FontStatus::Layout,
         []
         {
             return Sealed([](Bytes& b) { Put32(b, BitmapsOffset, Get32(b, BitmapsOffset) - 20); });
         }},
        {"bitmaps over text", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, BitmapsSize, Get32(b, BitmapsSize) + 4); }); }},
        {"text out of order", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TextOffset, Get32(b, BitmapsOffset)); }); }},
        {"text past the end", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TextSize, Get32(b, TextSize) + 1); }); }},
        {"text size wraps", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, TextSize, 0xFFFFFFFFU); }); }},
        {"duplicate code point", FontStatus::Order,
         [=]
         {
             return Sealed([=](Bytes& b)
                           { Put32(b, RecordOffset(b, 5), Get32(b, RecordOffset(b, 4))); });
         }},
        {"descending code points", FontStatus::Order,
         [=]
         {
             return Sealed([=](Bytes& b)
                           { Put32(b, RecordOffset(b, 5), Get32(b, RecordOffset(b, 3))); });
         }},
        {"code point past U+10FFFF", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put32(b, RecordOffset(b, last), 0x110000); }); }},
        {"surrogate", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put32(b, RecordOffset(b, last - 1), 0xD800); }); }},
        {"last surrogate", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put32(b, RecordOffset(b, last - 1), 0xDFFF); }); }},
        {"bitmap one byte past its section", FontStatus::Range,
         [=]
         {
             return Sealed(
                 [=](Bytes& b)
                 { Put32(b, RecordOffset(b, last) + 4, Get32(b, RecordOffset(b, last) + 4) + 1); });
         }},
        {"bitmap far past its section", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put32(b, RecordOffset(b, 5) + 4, 0xFFFFFFF0U); }); }},
        {"width over 256", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put16(b, RecordOffset(b, empty) + 8, 257); }); }},
        {"height over 256", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put16(b, RecordOffset(b, empty) + 10, 257); }); }},
        {"left bearing over 4 lines", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put16(b, RecordOffset(b, 5) + 12, 65); }); }},
        {"left bearing under -4 lines", FontStatus::Range,
         [=]
         {
             return Sealed([=](Bytes& b)
                           { Put16(b, RecordOffset(b, 5) + 12, static_cast<uint16_t>(-65)); });
         }},
        {"top over 4 lines", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put16(b, RecordOffset(b, 5) + 14, 65); }); }},
        {"advance over 4 lines", FontStatus::Range,
         [=] { return Sealed([=](Bytes& b) { Put16(b, RecordOffset(b, 5) + 16, 65); }); }},
        {"monospace advance", FontStatus::Range,
         []
         {
             return Sealed(
                 [](Bytes& b)
                 {
                     Put32(b, Flags, 1);
                     Put16(b, CellWidth, 6);
                 });
         }},
        {"monospace without a cell", FontStatus::Range,
         [] { return Sealed([](Bytes& b) { Put32(b, Flags, 1); }); }},
        {"proportional with a cell", FontStatus::Range,
         [] { return Sealed([](Bytes& b) { Put16(b, CellWidth, 6); }); }},
        {"ascent plus descent", FontStatus::Range,
         [] { return Sealed([](Bytes& b) { Put16(b, Ascent, 14); }); }},
        {"negative ascent", FontStatus::Range,
         [] { return Sealed([](Bytes& b) { Put16(b, Ascent, static_cast<uint16_t>(-1)); }); }},
        {"no record 0", FontStatus::Missing,
         [=] { return Sealed([=](Bytes& b) { Put32(b, RecordOffset(b, 0), 1); }); }},
        {"glyph count without a table", FontStatus::Layout,
         [] { return Sealed([](Bytes& b) { Put32(b, GlyphCount, 0); }); }},
        {"no records", FontStatus::Missing,
         []
         {
             return Sealed(
                 [](Bytes& b)
                 {
                     Put32(b, GlyphCount, 0);
                     Put32(b, TableSize, 0);
                 });
         }},
        {"unterminated text", FontStatus::Text,
         [] { return Sealed([](Bytes& b) { b[b.size() - 1] = 'x'; }); }},
        {"name runs into notice", FontStatus::Text,
         [] { return Sealed([](Bytes& b) { b[Get32(b, TextOffset) + 7] = 'x'; }); }},
        {"overlong name", FontStatus::Text,
         []
         {
             return Sealed(
                 [](Bytes& b)
                 {
                     b[Get32(b, TextOffset)] = 0xC0;
                     b[Get32(b, TextOffset) + 1] = 0x80;
                 });
         }},
        {"surrogate in notice", FontStatus::Text,
         []
         {
             return Sealed(
                 [](Bytes& b)
                 {
                     const size_t at = Get32(b, TextOffset) + 8;
                     b[at] = 0xED;
                     b[at + 1] = 0xA0;
                     b[at + 2] = 0x80;
                 });
         }},
    };
    for (const Refusal& refusal : cases)
    {
        INFO(refusal.name);
        const Bytes bytes = refusal.make();
        FontView view;
        const FontStatus status = Parse(bytes, &view);
        CHECK(Text::FontStatusName(status) == std::string(Text::FontStatusName(refusal.expected)));
        CHECK_FALSE(view.Valid());
    }
}

TEST_CASE("limits are accepted at the boundary", "[font]")
{
    const auto accepted = [](const std::function<void(Bytes&)>& edit)
    { return Parse(Sealed(edit)) == FontStatus::Ok; };
    CHECK(accepted([](Bytes& b) { Put16(b, RecordOffset(b, 5) + 12, 64); }));
    CHECK(
        accepted([](Bytes& b) { Put16(b, RecordOffset(b, 5) + 12, static_cast<uint16_t>(-64)); }));
    CHECK(accepted([](Bytes& b) { Put16(b, RecordOffset(b, 5) + 14, 64); }));
    CHECK(accepted([](Bytes& b) { Put16(b, RecordOffset(b, 5) + 16, 64); }));
    CHECK(accepted(
        [](Bytes& b)
        {
            Put16(b, Ascent, 16);
            Put16(b, Ascent + 2, 0);
        }));
    CHECK(accepted(
        [](Bytes& b)
        {
            const size_t index = IndexOf(0x20);
            Put16(b, RecordOffset(b, index) + 8, 256);
            Put16(b, RecordOffset(b, index) + 10, 0);
        }));
}

TEST_CASE("when a file has two defects the earlier refusal wins", "[font]")
{
    const auto status = [](const std::function<void(Bytes&)>& edit, bool seal)
    {
        Bytes bytes = Support::BuildFont(Support::SampleFont());
        edit(bytes);
        if (seal)
        {
            Support::Seal(bytes);
        }
        return std::string(Text::FontStatusName(Parse(bytes)));
    };
    CHECK(status(
              [](Bytes& b)
              {
                  b[0] = 'X';
                  Put16(b, Version, 9);
              },
              true) == "magic");
    CHECK(status(
              [](Bytes& b)
              {
                  Put16(b, Version, 9);
                  Put16(b, HeaderSize, 1);
              },
              true) == "version");
    CHECK(status(
              [](Bytes& b)
              {
                  Put16(b, HeaderSize, 1);
                  b[Reserved] = 1;
              },
              true) == "header");
    CHECK(status(
              [](Bytes& b)
              {
                  b[Reserved] = 1;
                  Put32(b, FileSize, 1);
              },
              true) == "reserved");
    CHECK(status(
              [](Bytes& b)
              {
                  Put32(b, FileSize, 1);
                  b[100] ^= 1;
              },
              false) == "length");
    CHECK(status([](Bytes& b) { Put32(b, TableOffset, 66); }, false) == "checksum");
    CHECK(status([](Bytes& b) { Put32(b, TableOffset, 66); }, true) == "layout");
    CHECK(status(
              [](Bytes& b)
              {
                  Put32(b, TextOffset, Get32(b, BitmapsOffset));
                  Put32(b, RecordOffset(b, 5), 0);
              },
              true) == "layout");
    CHECK(status(
              [](Bytes& b)
              {
                  Put16(b, RecordOffset(b, 7) + 18, 1);
                  Put32(b, RecordOffset(b, 5), Get32(b, RecordOffset(b, 3)));
              },
              true) == "reserved");
    CHECK(status(
              [](Bytes& b)
              {
                  Put32(b, RecordOffset(b, 5), Get32(b, RecordOffset(b, 4)));
                  Put16(b, RecordOffset(b, 6) + 8, 300);
              },
              true) == "order");
    CHECK(status(
              [](Bytes& b)
              {
                  Put32(b, RecordOffset(b, 0), 1);
                  Put16(b, RecordOffset(b, 6) + 12, 300);
              },
              true) == "range");
    CHECK(status(
              [](Bytes& b)
              {
                  Put32(b, RecordOffset(b, 0), 1);
                  b[b.size() - 1] = 'x';
              },
              true) == "missing");
}

TEST_CASE("every truncation and an appended byte is refused", "[font]")
{
    const Bytes bytes = Support::BuildFont(Support::SampleFont());
    for (size_t length = 0; length < bytes.size(); ++length)
    {
        FontView view;
        const FontStatus status = FontView::Parse(bytes.data(), length, &view);
        INFO("length " << length);
        CHECK(status != FontStatus::Ok);
        if (length < 64)
        {
            CHECK(status == FontStatus::Short);
        }
        else
        {
            CHECK(status == FontStatus::Length);
        }
    }
    Bytes longer = bytes;
    longer.push_back(0);
    CHECK(Parse(longer) == FontStatus::Length);
}

TEST_CASE("a flipped byte anywhere past the header's own checks is a checksum failure", "[font]")
{
    const Bytes bytes = Support::BuildFont(Support::SampleFont());
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        Bytes damaged = bytes;
        damaged[index] ^= 0x55;
        INFO("byte " << index);
        const FontStatus status = Parse(damaged);
        CHECK(status != FontStatus::Ok);
        if (index >= Support::FontOffset::Checksum)
        {
            CHECK(status == FontStatus::Checksum);
        }
    }
}

TEST_CASE("blending is exact at both ends and per channel between", "[font]")
{
    CHECK(Text::Blend(0x204060, 0xE0A000, 0) == 0x204060);
    CHECK(Text::Blend(0x204060, 0xE0A000, 15) == 0xE0A000);
    CHECK(Text::Blend(0x000000, 0xFFFFFF, 1) == 0x111111);
    CHECK(Text::Blend(0x000000, 0xFFFFFF, 7) == 0x777777);
    CHECK(Text::Blend(0x000000, 0xFFFFFF, 8) == 0x888888);
    CHECK(Text::Blend(0xFFFFFF, 0x000000, 8) == 0x777777);
    CHECK(Text::Blend(0x204060, 0xE0A000, 5) == 0x606040);
    CHECK(Text::Blend(0x102030, 0xF0E0D0, 14) == 0xE1D3C5);
    CHECK(Text::Blend(0x204060, 0xE0A000, 16) == 0xE0A000);
    for (uint32_t color = 0; color < 0x1000000; color += 0x010305)
    {
        CHECK(Text::Blend(color, color, 9) == color);
    }
}

TEST_CASE("every committed font parses and has its family's shape", "[font]")
{
    const std::vector<std::pair<std::string, bool>> families = {{"mono", true}, {"sans", false}};
    const std::vector<int> sizes = {16, 20, 24, 32, 48, 64};
    for (const auto& [family, monospace] : families)
    {
        for (const int size : sizes)
        {
            if (monospace && size > 32)
            {
                continue;
            }
            const std::string name = family + "-" + std::to_string(size);
            INFO(name);
            const Bytes bytes = ReadFile(FontPath(name + ".ffnt"));
            REQUIRE(bytes.size() <= FontView::MaximumFileSize);
            FontView view;
            REQUIRE(Parse(bytes, &view) == FontStatus::Ok);
            CHECK(std::string(view.Name()) == family + " " + std::to_string(size));
            CHECK(std::string(view.Notice()).find("SIL Open Font License 1.1") !=
                  std::string::npos);
            CHECK(std::string(view.Name()).find("Inter") == std::string::npos);
            CHECK(std::string(view.Name()).find("JetBrains") == std::string::npos);
            CHECK(view.Monospace() == monospace);
            CHECK(view.LineHeight() == size);
            CHECK(view.Ascent() + view.Descent() == size);
            CHECK(view.CellWidth() == (monospace ? size / 2 : 0));
            CHECK(view.Find(0).code_point == 0);
            for (char32_t code_point = 0x20; code_point <= 0x7E; ++code_point)
            {
                CHECK(view.Contains(code_point));
            }
            for (const char32_t code_point :
                 {0xE9, 0x151, 0x3B1, 0x416, 0x2192, 0x2500, 0x2588, 0x20AC})
            {
                CHECK(view.Contains(code_point));
            }
            CHECK_FALSE(view.Contains(0x378));
            CHECK_FALSE(view.Contains(0x1F));
            CHECK_FALSE(view.Contains(0x4E2D));
            CHECK(view.GlyphCount() > 600);
        }
    }
}

TEST_CASE("the console font fits its 8x16 cell and equals mono-16.ffnt", "[font]")
{
    const Bytes bytes = ReadFile(FontPath("mono-16.ffnt"));
    REQUIRE(ConsoleFontSize == bytes.size());
    CHECK(std::memcmp(ConsoleFontData, bytes.data(), bytes.size()) == 0);

    FontView view;
    REQUIRE(FontView::Parse(ConsoleFontData, ConsoleFontSize, &view) == FontStatus::Ok);
    CHECK(view.CellWidth() == ConsoleFontCellWidth);
    CHECK(view.LineHeight() == ConsoleFontCellHeight);
    for (uint32_t index = 0; index < view.GlyphCount(); ++index)
    {
        const Text::Glyph glyph = view.GlyphAt(index);
        INFO("code point " << static_cast<uint32_t>(glyph.code_point));
        CHECK(glyph.advance == ConsoleFontCellWidth);
        if (glyph.width == 0)
        {
            continue;
        }
        const int top = view.Ascent() - glyph.top;
        CHECK(glyph.left >= 0);
        CHECK(glyph.left + glyph.width <= ConsoleFontCellWidth);
        CHECK(top >= 0);
        CHECK(top + glyph.height <= ConsoleFontCellHeight);
    }
}

namespace
{

struct Rendering
{
    int width;
    int height;
    uint32_t checksum;
};

Rendering Render(const FontView& view, const std::string& text, uint32_t background,
                 uint32_t foreground)
{
    std::vector<Text::Glyph> glyphs;
    size_t offset = 0;
    while (offset < text.size())
    {
        size_t used = 0;
        const char32_t code_point =
            Text::DecodeNext(text.data() + offset, text.size() - offset, &used);
        offset += used;
        if (code_point < 0x20 || code_point == 0x7F)
        {
            continue;
        }
        glyphs.push_back(view.Find(code_point));
    }
    constexpr int Margin = 4;
    int advance = 0;
    for (const Text::Glyph& glyph : glyphs)
    {
        advance += glyph.advance;
    }
    const int width = advance + 2 * Margin;
    const int height = view.LineHeight() + 2 * Margin;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height),
                                 background);
    int pen = Margin;
    const int baseline = Margin + view.Ascent();
    for (const Text::Glyph& glyph : glyphs)
    {
        for (int y = 0; y < glyph.height; ++y)
        {
            for (int x = 0; x < glyph.width; ++x)
            {
                const int px = pen + glyph.left + x;
                const int py = baseline - glyph.top + y;
                const uint32_t coverage = view.Coverage(glyph, x, y);
                if (coverage != 0 && px >= 0 && px < width && py >= 0 && py < height)
                {
                    uint32_t& pixel = pixels[static_cast<size_t>(py * width + px)];
                    pixel = Text::Blend(pixel, foreground, coverage);
                }
            }
        }
        pen += glyph.advance;
    }
    uint32_t hash = 2166136261U;
    for (const uint32_t pixel : pixels)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            hash = (hash ^ ((pixel & 0xFFFFFF) >> shift & 0xFF)) * 16777619U;
        }
    }
    return {width, height, hash};
}

} // namespace

TEST_CASE("renderings match the checksums written by tools/fonts/ffnt.py", "[font]")
{
    const Bytes reference = ReadFile(FEATHEROS_FONT_REFERENCE);
    std::istringstream lines(std::string(reference.begin(), reference.end()));
    std::string line;
    int checked = 0;
    while (std::getline(lines, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::vector<std::string> fields;
        size_t start = 0;
        for (int field = 0; field < 4; ++field)
        {
            const size_t tab = line.find('\t', start);
            REQUIRE(tab != std::string::npos);
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        const std::string text = line.substr(start);
        const Bytes bytes = ReadFile(FontPath(fields[0] + ".ffnt"));
        FontView view;
        REQUIRE(Parse(bytes, &view) == FontStatus::Ok);
        const Rendering rendering = Render(view, text, 0x203050, 0xF0E0C0);
        INFO(fields[0] << ": " << text);
        CHECK(rendering.width == std::stoi(fields[2]));
        CHECK(rendering.height == std::stoi(fields[3]));
        std::ostringstream checksum;
        checksum << std::hex << std::nouppercase;
        checksum.width(8);
        checksum.fill('0');
        checksum << rendering.checksum;
        CHECK(checksum.str() == fields[1]);
        ++checked;
    }
    CHECK(checked == 60);
}
