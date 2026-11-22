#include <catch2/catch_test_macros.hpp>

#include "kernel/framebuffer/console_font.h"
#include "user/gfx_check.h"
#include "user/graphics/font.h"
#include "user/graphics/surface.h"
#include "user/text/font_data.h"
#include "user/text/utf8.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
namespace Text = FeatherOS::Text;
using Graphics::Font;
using Graphics::Surface;
using Text::FontStatus;

namespace
{

constexpr uint32_t Background = 0x203050;
constexpr uint32_t Foreground = 0xF0E0C0;

std::string FontPath(const std::string& name)
{
    return std::string(FEATHEROS_FONT_DIRECTORY) + "/" + name + ".ffnt";
}

Font LoadFont(const std::string& name)
{
    Font font;
    REQUIRE(font.Load(FontPath(name).c_str()) == FontStatus::Ok);
    return font;
}

std::vector<uint32_t> Pixels(const Surface& surface)
{
    return std::vector<uint32_t>(surface.Pixels(),
                                 surface.Pixels() + static_cast<size_t>(surface.Width()) *
                                                        static_cast<size_t>(surface.Height()));
}

uint32_t ExpectedBlend(uint32_t background, uint32_t foreground, uint32_t coverage)
{
    uint32_t result = 0;
    const uint32_t alpha = coverage * 17;
    for (int shift = 0; shift < 24; shift += 8)
    {
        const uint32_t b = background >> shift & 0xFF;
        const uint32_t f = foreground >> shift & 0xFF;
        result |= ((f * alpha + b * (255 - alpha) + 127) / 255) << shift;
    }
    return result;
}

} // namespace

TEST_CASE("a font loads from a file, from memory, or not at all", "[graphics][font]")
{
    Font empty;
    CHECK_FALSE(empty.Valid());
    CHECK(empty.MeasureText("abc") == 0);
    CHECK(empty.Advance('a') == 0);
    CHECK_FALSE(empty.HasGlyph('a'));
    CHECK(std::string(empty.Name()).empty());

    Font font;
    CHECK(font.Load(FontPath("sans-24").c_str()) == FontStatus::Ok);
    CHECK(font.Valid());
    CHECK_FALSE(font.Monospace());
    CHECK(font.LineHeight() == 24);
    CHECK(font.Ascent() + font.Descent() == 24);
    CHECK(std::string(font.Name()) == "sans 24");
    CHECK(std::string(font.Notice()).find("SIL Open Font License 1.1") != std::string::npos);

    CHECK(font.Load("/nonexistent/font.ffnt") == FontStatus::Short);
    CHECK_FALSE(font.Valid());
    CHECK(font.Load(nullptr) == FontStatus::Short);
    CHECK(font.LoadNamed("../sans-24") == FontStatus::Short);
    CHECK(font.LoadNamed(nullptr) == FontStatus::Short);
    CHECK(font.LoadNamed("sans-999") == FontStatus::Short);
    CHECK_FALSE(font.Valid());

    std::ifstream file(FontPath("mono-20"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    REQUIRE(font.LoadMemory(bytes.data(), bytes.size()) == FontStatus::Ok);
    CHECK(font.Monospace());
    CHECK(font.CellWidth() == 10);

    bytes.push_back(0);
    CHECK(font.LoadMemory(bytes.data(), bytes.size()) == FontStatus::Length);
    CHECK_FALSE(font.Valid());
    CHECK(std::string(Text::FontStatusName(FontStatus::Length)) == "length");
    CHECK(font.LoadMemory(nullptr, 0) == FontStatus::Short);
}

namespace
{

struct TemporaryFile
{
    std::string path;

    explicit TemporaryFile(const std::vector<uint8_t>& bytes)
    {
        path = (std::filesystem::temp_directory_path() /
                ("featheros_font_test_" + std::to_string(counter++) + ".ffnt"))
                   .string();
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    ~TemporaryFile()
    {
        std::remove(path.c_str());
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    static inline int counter = 0;
};

std::vector<uint8_t> FontBytes(const std::string& name)
{
    std::ifstream file(FontPath(name), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Load sizes its buffer by the file and refuses what is wrong with it", "[graphics][font]")
{
    Font font;
    const std::vector<uint8_t> bytes = FontBytes("sans-16");
    REQUIRE_FALSE(bytes.empty());
    {
        const TemporaryFile exact(bytes);
        CHECK(font.Load(exact.path.c_str()) == FontStatus::Ok);
        CHECK(font.Valid());
    }
    {
        std::vector<uint8_t> longer = bytes;
        longer.push_back(0);
        const TemporaryFile appended(longer);
        CHECK(font.Load(appended.path.c_str()) == FontStatus::Length);
        CHECK_FALSE(font.Valid());
    }
    {
        std::vector<uint8_t> shorter(bytes.begin(), bytes.end() - 1);
        const TemporaryFile truncated(shorter);
        CHECK(font.Load(truncated.path.c_str()) == FontStatus::Length);
        const TemporaryFile stub(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 10));
        CHECK(font.Load(stub.path.c_str()) == FontStatus::Short);
        const TemporaryFile empty{std::vector<uint8_t>()};
        CHECK(font.Load(empty.path.c_str()) == FontStatus::Short);
    }
    {
        std::vector<uint8_t> limit(Text::FontView::MaximumFileSize, 0);
        const TemporaryFile at_limit(limit);
        CHECK(font.Load(at_limit.path.c_str()) == FontStatus::Magic);
        limit.push_back(0);
        const TemporaryFile over_limit(limit);
        CHECK(font.Load(over_limit.path.c_str()) == FontStatus::Size);
        CHECK_FALSE(font.Valid());
    }
    CHECK(font.Load(std::filesystem::temp_directory_path().string().c_str()) ==
          FontStatus::Short);
}

TEST_CASE("a font moves and the source is left empty", "[graphics][font]")
{
    Font source = LoadFont("sans-16");
    const int32_t width = source.MeasureText("Move");
    Font moved(std::move(source));
    CHECK_FALSE(source.Valid());
    CHECK(moved.Valid());
    CHECK(moved.MeasureText("Move") == width);

    Font other = LoadFont("mono-16");
    other = std::move(moved);
    CHECK_FALSE(moved.Valid());
    CHECK(other.MeasureText("Move") == width);
}

TEST_CASE("the console font is compiled in and is mono-16", "[graphics][font]")
{
    const Font* console = Font::Console();
    REQUIRE(console != nullptr);
    REQUIRE(console->Valid());
    CHECK(console == Font::Console());
    CHECK(console->Monospace());
    CHECK(console->CellWidth() == ConsoleFontCellWidth);
    CHECK(console->LineHeight() == ConsoleFontCellHeight);
    CHECK(std::string(console->Name()) == "mono 16");
    CHECK(console->MeasureText("Hello, wörld") == 12 * ConsoleFontCellWidth);
}

TEST_CASE("text is measured by code point, with controls and invalid bytes", "[graphics][font]")
{
    const Font mono = LoadFont("mono-24");
    CHECK(mono.MeasureText("") == 0);
    CHECK(mono.MeasureText(nullptr) == 0);
    CHECK(mono.MeasureText("abc") == 36);
    CHECK(mono.MeasureText("h\xC3\xA9\xC3\xA9") == 36);
    CHECK(mono.MeasureText("a\tb\x01\x7F\x1F") == 24);
    CHECK(mono.MeasureText("\t\n\r\x7F") == 0);

    const Font sans = LoadFont("sans-32");
    int32_t sum = 0;
    for (const char c : std::string("Wiq,. ij"))
    {
        sum += sans.Advance(static_cast<uint8_t>(c));
    }
    CHECK(sum == sans.MeasureText("Wiq,. ij"));
    CHECK(sans.Advance('W') != sans.Advance('i'));
    CHECK(sans.Advance('\t') == 0);
    CHECK(sans.Advance(0x7F) == 0);
    CHECK(sans.Advance(0x1F) == 0);

    // A code point the font lacks takes the width of its missing box; so does a C1 control.
    CHECK_FALSE(sans.HasGlyph(0x4E2D));
    CHECK(sans.HasGlyph('W'));
    CHECK(sans.Advance(0x4E2D) == sans.Advance(0x378));
    CHECK(sans.Advance(0x4E2D) > 0);
    CHECK(sans.MeasureText("\xE4\xB8\xAD") == sans.Advance(0x4E2D));
    CHECK(sans.MeasureText("\xC2\x85") == sans.Advance(0x4E2D));

    // Invalid UTF-8 is U+FFFD per maximal subpart.
    CHECK(sans.HasGlyph(0xFFFD));
    CHECK(sans.MeasureText("\xFF") == sans.Advance(0xFFFD));
    CHECK(sans.MeasureText("\xE2\x82") == sans.Advance(0xFFFD));
    CHECK(sans.MeasureText("a\xFF" "b") ==
          sans.Advance('a') + sans.Advance(0xFFFD) + sans.Advance('b'));
    CHECK(sans.Advance(0xFFFD) != sans.Advance(0x4E2D));
}

TEST_CASE("text is drawn with coverage blended over what is there", "[graphics][font]")
{
    const Font font = LoadFont("sans-32");
    Surface surface(64, 48);
    surface.Fill(Background);
    const int32_t end = surface.DrawText(&font, 8, 6, "H", Foreground);
    CHECK(end == 8 + font.Advance('H'));

    const Text::FontView& view = font.View();
    const Text::Glyph glyph = view.Find('H');
    bool full = false;
    bool partial = false;
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            const int32_t gx = x - (8 + glyph.left);
            const int32_t gy = y - (6 + font.Ascent() - glyph.top);
            const uint32_t coverage = view.Coverage(glyph, gx, gy);
            INFO("pixel " << x << "," << y);
            CHECK(surface.PixelAt(x, y) == ExpectedBlend(Background, Foreground, coverage));
            full = full || coverage == 15;
            partial = partial || (coverage != 0 && coverage != 15);
        }
    }
    CHECK(full);
    CHECK(partial);

    // The top row of the line box is y, with the baseline ascent rows below it.
    int32_t first_row = -1;
    for (int32_t y = 0; y < surface.Height() && first_row < 0; ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            if (surface.PixelAt(x, y) != Background)
            {
                first_row = y;
                break;
            }
        }
    }
    CHECK(first_row == 6 + font.Ascent() - glyph.top);
}

TEST_CASE("full coverage writes the color exactly and the high byte is ignored",
          "[graphics][font]")
{
    const Font font = LoadFont("mono-32");
    Surface surface(40, 40);
    surface.Fill(0x123456);
    surface.DrawText(&font, 4, 4, "\xE2\x96\x88", 0xFF00FF00);
    size_t exact = 0;
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            const uint32_t pixel = surface.PixelAt(x, y);
            CHECK(pixel <= 0xFFFFFF);
            exact += pixel == 0x00FF00 ? 1 : 0;
        }
    }
    CHECK(exact == static_cast<size_t>(font.CellWidth()) * static_cast<size_t>(font.LineHeight()));
}

TEST_CASE("text that is clipped matches the same text on a larger surface", "[graphics][font]")
{
    const Font font = LoadFont("sans-24");
    const char* text = "Wg \xC3\x89 \xCE\xA9\xCE\xBC \xD0\x96 \xE2\x94\x8C\xE2\x94\x80";
    constexpr int32_t Border = 80;
    constexpr int32_t Width = 70;
    constexpr int32_t Height = 40;
    for (int32_t dy = -30; dy <= Height + 2; dy += 8)
    {
        for (int32_t dx = -Width - 4; dx <= Width + 4; dx += 7)
        {
            Surface big(Width + 2 * Border + 160, Height + 2 * Border);
            big.Fill(Background);
            const int32_t big_end = big.DrawText(&font, Border + dx, Border + dy, text, Foreground);
            Surface small(Width, Height);
            small.Fill(Background);
            const int32_t small_end = small.DrawText(&font, dx, dy, text, Foreground);
            INFO("at " << dx << "," << dy);
            CHECK(small_end == dx + font.MeasureText(text));
            CHECK(big_end == Border + small_end);
            for (int32_t y = 0; y < Height; ++y)
            {
                for (int32_t x = 0; x < Width; ++x)
                {
                    REQUIRE(small.PixelAt(x, y) == big.PixelAt(Border + x, Border + y));
                }
            }
        }
    }
}

TEST_CASE("the last column and row are drawn and nothing past them", "[graphics][font]")
{
    const Font font = LoadFont("mono-32");
    const Text::Glyph block = font.View().Find(0x2588);
    REQUIRE(block.width == font.CellWidth());
    const int32_t top = font.Ascent() - block.top;
    for (const int32_t width : {7, 8, 9})
    {
        Surface surface(width, 40);
        surface.Fill(Background);
        surface.DrawText(&font, 0, 0, "\xE2\x96\x88", Foreground);
        CHECK(surface.PixelAt(width - 1, top) == Foreground);
        CHECK(surface.PixelAt(width - 1, top + block.height - 1) == Foreground);
        CHECK(surface.PixelAt(0, top + block.height) == Background);
    }
    const int32_t bottom = top + block.height;
    for (const int32_t height : {bottom - 1, bottom, bottom + 1})
    {
        Surface surface(24, height);
        surface.Fill(Background);
        surface.DrawText(&font, 0, 0, "\xE2\x96\x88", Foreground);
        const int32_t last_inked = std::min(height, bottom) - 1;
        CHECK(surface.PixelAt(block.left, last_inked) == Foreground);
        CHECK(surface.PixelAt(block.left + block.width - 1, last_inked) == Foreground);
        CHECK(surface.PixelAt(block.left + block.width, last_inked) == Background);
        if (height > bottom)
        {
            CHECK(surface.PixelAt(block.left, bottom) == Background);
        }
    }
}

TEST_CASE("positions at the limits of 32 bits draw nothing and do not wrap", "[graphics][font]")
{
    const Font font = LoadFont("sans-16");
    Surface surface(32, 32);
    surface.Fill(Background);
    const std::vector<uint32_t> before = Pixels(surface);
    const char* text = "WWWWWW\xE2\x94\x80";

    CHECK(surface.DrawText(&font, INT32_MAX, 4, text, Foreground) == INT32_MAX);
    CHECK(surface.DrawText(&font, INT32_MAX - 2, 4, text, Foreground) == INT32_MAX);
    CHECK(surface.DrawText(&font, INT32_MAX - 20, 4, "W", Foreground) == INT32_MAX - 20 + font.Advance('W'));
    CHECK(surface.DrawText(&font, 4, INT32_MAX, text, Foreground) > 4);
    CHECK(surface.DrawText(&font, 4, INT32_MAX - 3, text, Foreground) > 4);
    CHECK(surface.DrawText(&font, 4, INT32_MIN, text, Foreground) > 4);
    CHECK(surface.DrawText(&font, 4, INT32_MIN + 3, text, Foreground) > 4);
    CHECK(surface.DrawText(&font, INT32_MIN, 4, text, Foreground) < 0);
    CHECK(surface.DrawText(&font, INT32_MIN, INT32_MIN, text, Foreground) < 0);
    CHECK(surface.DrawText(&font, INT32_MAX, INT32_MAX, text, Foreground) == INT32_MAX);
    CHECK(Pixels(surface) == before);

    // A string whose width passes 32 bits still gives a saturated end and clips the rest.
    const std::string long_text(70000, 'W');
    CHECK(surface.DrawText(&font, 0, 0, long_text.c_str(), Foreground) > 0);
    CHECK(font.MeasureText(long_text.c_str()) == 70000 * font.Advance('W'));
}

TEST_CASE("controls draw nothing and do not advance", "[graphics][font]")
{
    const Font font = LoadFont("sans-20");
    Surface plain(80, 30);
    Surface controlled(80, 30);
    plain.Fill(Background);
    controlled.Fill(Background);
    const int32_t plain_end = plain.DrawText(&font, 2, 3, "ab", Foreground);
    const int32_t controlled_end =
        controlled.DrawText(&font, 2, 3, "\x01" "a\t\x7F" "b\n\x1F", Foreground);
    CHECK(controlled_end == plain_end);
    CHECK(Pixels(controlled) == Pixels(plain));

    Surface untouched(20, 20);
    untouched.Fill(Background);
    CHECK(untouched.DrawText(&font, 5, 5, "\t\n\x01\x7F", Foreground) == 5);
    CHECK(Pixels(untouched) == std::vector<uint32_t>(400, Background));
}

TEST_CASE("a missing code point draws record 0 and invalid bytes draw U+FFFD", "[graphics][font]")
{
    const Font font = LoadFont("sans-32");
    auto draw = [&](const char* text) {
        Surface surface(60, 40);
        surface.Fill(Background);
        const int32_t end = surface.DrawText(&font, 4, 4, text, Foreground);
        return std::make_pair(end, Pixels(surface));
    };
    const auto cjk = draw("\xE4\xB8\xAD");
    CHECK(cjk == draw("\xCD\xB8"));
    CHECK(cjk.second != draw("W").second);
    CHECK(cjk.second != draw("\xEF\xBF\xBD").second);

    const auto replacement = draw("\xEF\xBF\xBD");
    CHECK(draw("\xFF") == replacement);
    CHECK(draw("\xC0\x80").second != replacement.second);
    CHECK(draw("\xE2\x82") == replacement);
    CHECK(draw("\xC3") == replacement);
    CHECK(replacement.second != std::vector<uint32_t>(2400, Background));
}

TEST_CASE("DrawTextOn fills the measured box first", "[graphics][font]")
{
    const Font font = LoadFont("sans-24");
    Surface surface(100, 40);
    surface.Fill(0x000000);
    const int32_t end = surface.DrawTextOn(&font, 6, 5, "Box", Foreground, Background);
    const int32_t width = font.MeasureText("Box");
    CHECK(end == 6 + width);
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            const bool inside = x >= 6 && x < 6 + width && y >= 5 && y < 5 + font.LineHeight();
            if (!inside)
            {
                CHECK(surface.PixelAt(x, y) == 0);
            }
        }
    }
    Surface reference(100, 40);
    reference.Fill(0x000000);
    reference.FillRectangle(6, 5, width, font.LineHeight(), Background);
    reference.DrawText(&font, 6, 5, "Box", Foreground);
    CHECK(Pixels(surface) == Pixels(reference));

    Surface other(100, 40);
    other.Fill(0x000000);
    CHECK(other.DrawTextOn(&font, -3000, 5, "Box", Foreground, Background) == -3000 + width);
    CHECK(Pixels(other) == std::vector<uint32_t>(4000, 0));
}

TEST_CASE("no font, no text, or no pixels draws nothing", "[graphics][font]")
{
    const Font font = LoadFont("sans-16");
    const Font invalid;
    Surface surface(30, 30);
    surface.Fill(Background);
    const std::vector<uint32_t> before = Pixels(surface);
    CHECK(surface.DrawText(nullptr, 3, 4, "x", Foreground) == 3);
    CHECK(surface.DrawText(&invalid, 3, 4, "x", Foreground) == 3);
    CHECK(surface.DrawText(&font, 3, 4, nullptr, Foreground) == 3);
    CHECK(surface.DrawTextOn(nullptr, 3, 4, "x", Foreground, 0) == 3);
    CHECK(surface.DrawTextOn(&invalid, 3, 4, "x", Foreground, 0) == 3);
    CHECK(surface.DrawTextOn(&font, 3, 4, nullptr, Foreground, 0) == 3);
    CHECK(Pixels(surface) == before);

    Surface none(0, 0);
    REQUIRE_FALSE(none.Valid());
    CHECK(none.DrawText(&font, 1, 1, "x", Foreground) == 1 + font.Advance('x'));
}

TEST_CASE("renderings through Surface::DrawText match tools/fonts/ffnt.py", "[graphics][font]")
{
    std::ifstream file(FEATHEROS_FONT_REFERENCE, std::ios::binary);
    REQUIRE(file.good());
    std::string line;
    int checked = 0;
    while (std::getline(file, line))
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
        const Font font = LoadFont(fields[0]);
        Surface surface(font.MeasureText(text.c_str()) + 8, font.LineHeight() + 8);
        REQUIRE(surface.Valid());
        surface.Fill(Background);
        surface.DrawText(&font, 4, 4, text.c_str(), Foreground);
        INFO(fields[0] << ": " << text);
        CHECK(surface.Width() == std::stoi(fields[2]));
        CHECK(surface.Height() == std::stoi(fields[3]));
        std::ostringstream checksum;
        checksum.width(8);
        checksum.fill('0');
        checksum << std::hex << surface.Checksum();
        CHECK(checksum.str() == fields[1]);
        ++checked;
    }
    CHECK(checked == 70);
}

TEST_CASE("gfxcheck's plain-loop reference equals the library's picture", "[graphics][font]")
{
    namespace Check = FeatherOS::User::GfxCheck;
    const Font sans = LoadFont("sans-16");
    const Font* console = Font::Console();
    REQUIRE(console != nullptr);
    Surface picture(Check::FontPictureWidth, Check::FontPictureHeight);
    picture.Fill(Background);
    const int32_t end = picture.DrawText(&sans, 4, 4, Check::FontText(), Foreground);
    picture.DrawTextOn(&sans, 4, 26, Check::OnText(), 0x00FF00, 0x602020);
    picture.DrawText(console, 50, 26, Check::ConsoleText(), 0xFFFF00);
    picture.DrawText(console, 70, 40, "cut", 0xFF8080);
    CHECK(end == 157);
    CHECK(picture.Checksum() == 1648750494U);
    CHECK(picture.Checksum() == Check::ExpectedFontChecksum(FontPath("sans-16").c_str()));
    CHECK(Check::ExpectedFontChecksum("/nonexistent") == 0);
}
