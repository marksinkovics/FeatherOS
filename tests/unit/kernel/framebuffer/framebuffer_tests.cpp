#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"

extern "C"
{
#include "kernel/framebuffer/font.h"
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

limine_framebuffer MakeFramebuffer(uint8_t* pixels, uint64_t width, uint64_t height,
                                   uint64_t pitch, uint16_t bits_per_pixel)
{
    limine_framebuffer framebuffer{};
    framebuffer.address = pixels;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = pitch;
    framebuffer.bpp = bits_per_pixel;
    framebuffer.memory_model = LIMINE_FRAMEBUFFER_RGB;
    framebuffer.red_mask_size = bits_per_pixel == 16 ? 5 : 8;
    framebuffer.red_mask_shift = bits_per_pixel == 16 ? 11 : 16;
    framebuffer.green_mask_size = bits_per_pixel == 16 ? 6 : 8;
    framebuffer.green_mask_shift = bits_per_pixel == 16 ? 5 : 8;
    framebuffer.blue_mask_size = bits_per_pixel == 16 ? 5 : 8;
    framebuffer.blue_mask_shift = 0;
    return framebuffer;
}

} // namespace

TEST_CASE("every printable character has its own glyph")
{
    uint8_t index = 0;
    CHECK(indexForCharacter('!', &index) == 0);
    CHECK(index == 0);
    CHECK(indexForCharacter('~', &index) == 0);
    CHECK(index == 93);
    uint8_t apostrophe = 0;
    uint8_t backtick = 0;
    CHECK(indexForCharacter('\'', &apostrophe) == 0);
    CHECK(indexForCharacter('`', &backtick) == 0);
    CHECK(apostrophe != backtick);
    CHECK(indexForCharacter(' ', &index) == -1);
    CHECK(indexForCharacter('\0', &index) == -1);
    CHECK(indexForCharacter('\x7f', &index) == -1);
}

TEST_CASE("framebuffer initializes and draws masked RGB pixels")
{
    std::array<uint8_t, 16> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), 2, 2, 8, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));
    framebuffer.drawPixel(1, 1, 0x123456);
    framebuffer.draw();

    REQUIRE(pixels[12] == 0x56);
    REQUIRE(pixels[13] == 0x34);
    REQUIRE(pixels[14] == 0x12);
    REQUIRE(pixels[15] == 0x00);
}

TEST_CASE("framebuffer ignores pixels outside its bounds")
{
    std::array<uint8_t, 16> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), 2, 2, 8, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));
    framebuffer.drawPixel(2, 0, 0xFFFFFF);
    framebuffer.drawPixel(0, 2, 0xFFFFFF);
    framebuffer.draw();
    REQUIRE(pixels == std::array<uint8_t, 16>{});
}

TEST_CASE("framebuffer rejects invalid descriptors")
{
    Framebuffer framebuffer;
    limine_framebuffer descriptor{};
    REQUIRE_FALSE(framebuffer.init(&descriptor));

    std::array<uint8_t, 16> pixels{};
    descriptor = MakeFramebuffer(pixels.data(), 2, 2, 4, 32);
    REQUIRE_FALSE(framebuffer.init(&descriptor));
}

TEST_CASE("terminal rendering keeps padding around the status bar and text")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 80;
    constexpr uint64_t pitch = width * 4;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, pitch, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));

    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);
    terminal.SetStatus("shell", "1/4  Mon Oct 19 9:36 PM");
    terminal.Print("Boot status\n> ");
    terminal.SetCursorVisible(true);
    terminal.Print("x");
    terminal.EraseCharacter();

    for (uint64_t y = 0; y < FeatherOS::TermConfig::Padding * FONT_HEIGHT; ++y)
    {
        for (uint64_t x = 0; x < width * 4; ++x)
        {
            REQUIRE(pixels[y * pitch + x] == 0);
        }
    }
    for (uint64_t y = 0; y < height; ++y)
    {
        for (uint64_t x = 0; x < FeatherOS::TermConfig::Padding * FONT_WIDTH; ++x)
        {
            REQUIRE(pixels[y * pitch + x * 4] == 0);
        }
        for (uint64_t x = width - FeatherOS::TermConfig::Padding * FONT_WIDTH; x < width; ++x)
        {
            REQUIRE(pixels[y * pitch + x * 4] == 0);
        }
    }
    for (uint64_t y = height - FeatherOS::TermConfig::Padding * FONT_HEIGHT; y < height; ++y)
    {
        for (uint64_t x = 0; x < pitch; ++x)
        {
            REQUIRE(pixels[y * pitch + x] == 0);
        }
    }

    size_t lit_pixels = 0;
    for (uint8_t pixel : pixels)
    {
        lit_pixels += pixel != 0;
    }
    REQUIRE(lit_pixels > 0);
}

TEST_CASE("status text draws green and red dots in place of their marker bytes")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 80;
    constexpr uint64_t pitch = width * 4;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, pitch, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));
    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);

    const auto count_pixels = [&](uint32_t color)
    {
        size_t count = 0;
        for (uint64_t offset = 0; offset < pixels.size(); offset += 4)
        {
            const uint32_t pixel = pixels[offset] | (pixels[offset + 1] << 8) |
                                   (pixels[offset + 2] << 16);
            count += pixel == color;
        }
        return count;
    };
    const std::string green = {'1', '/', '4', ' ', ' ', FeatherOS::Term::StatusGreenDot};
    terminal.SetStatus("shell", green.c_str());
    REQUIRE(terminal.CharacterAt(0, terminal.Columns() - 2) == FeatherOS::Term::StatusGreenDot);
    REQUIRE(count_pixels(0x00A000) > 20);
    REQUIRE(count_pixels(0xD00000) == 0);

    const std::string red = {'1', '/', '4', ' ', ' ', FeatherOS::Term::StatusRedDot};
    terminal.SetStatus("shell", red.c_str());
    REQUIRE(count_pixels(0x00A000) == 0);
    REQUIRE(count_pixels(0xD00000) > 20);
}

TEST_CASE("terminals sharing a framebuffer draw only while shown")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 80;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, width * 4, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));

    FeatherOS::Term first;
    FeatherOS::Term second;
    first.Init(&framebuffer);
    second.Init(&framebuffer);
    second.SetShown(false);
    first.SetStatus("first", "");
    first.Print("first console");
    const auto first_pixels = pixels;

    second.SetStatus("second", "");
    second.Print("second console\nmore");
    REQUIRE(pixels == first_pixels);
    REQUIRE(second.CharacterAt(1, 0) == 's');
    REQUIRE(second.CursorRow() == 2);

    first.SetShown(false);
    second.SetShown(true);
    REQUIRE(second.Shown());
    REQUIRE(pixels != first_pixels);
    const auto second_pixels = pixels;
    first.Print(" and more");
    REQUIRE(pixels == second_pixels);

    // Showing it again repaints all of it, including what it printed while hidden.
    second.SetShown(false);
    first.SetShown(true);
    FeatherOS::Term reference;
    std::array<uint8_t, width * height * 4> reference_pixels{};
    limine_framebuffer reference_descriptor =
        MakeFramebuffer(reference_pixels.data(), width, height, width * 4, 32);
    Framebuffer reference_framebuffer;
    REQUIRE(reference_framebuffer.init(&reference_descriptor));
    reference.Init(&reference_framebuffer);
    reference.SetStatus("first", "");
    reference.Print("first console and more");
    REQUIRE(pixels == reference_pixels);
}

TEST_CASE("terminal places status text at the bar's edges and input inline")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 80;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, width * 4, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));

    // 40 columns and 5 rows, minus padding: a status row and two text rows of 38 columns.
    constexpr uint32_t columns = 38;
    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);
    terminal.SetStatus("upper", "2/4  Mon Oct 19 9:36 PM");
    const auto row_text = [&](uint32_t row)
    {
        std::string text;
        for (uint32_t column = 0; column < columns; ++column)
        {
            text += static_cast<char>(terminal.CharacterAt(row, column));
        }
        return text;
    };
    REQUIRE(row_text(0) == " upper        2/4  Mon Oct 19 9:36 PM ");

    terminal.Print("> ");
    terminal.Print("abc");
    terminal.EraseCharacter();
    REQUIRE(row_text(1).substr(0, 5) == "> ab ");

    // Erasing at the start of a wrapped row continues on the row above.
    terminal.Print("\n");
    terminal.Print(std::string(columns, 'w').c_str());
    terminal.EraseCharacter();
    REQUIRE(row_text(1) == std::string(columns - 1, 'w') + " ");

    // Scrolling moves only the text rows.
    terminal.Print("\nnext");
    REQUIRE(row_text(0) == " upper        2/4  Mon Oct 19 9:36 PM ");
    REQUIRE(row_text(1) == std::string(columns - 1, 'w') + " ");
    REQUIRE(row_text(2).substr(0, 4) == "next");

    // Left text is cut short rather than running into the right text.
    terminal.SetStatus("a-very-long-program-name-here", "2/4  Mon Oct 19 9:36 PM");
    REQUIRE(row_text(0) == " a-very-long- 2/4  Mon Oct 19 9:36 PM ");
}

TEST_CASE("incremental terminal rendering matches a full redraw")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 112;
    constexpr uint64_t pitch = width * 4;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, pitch, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));

    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);
    terminal.SetStatus("shell", "1/4  Mon Oct 19 9:36 PM");
    terminal.Print("> ");
    terminal.SetCursorVisible(true);
    // The burst scrolls past every text row; later single-line scrolls shift existing pixels,
    // including the cursor.
    terminal.Print("a\nb\nc\nd\ne\nf\ng\nh\n");
    char line[] = "log line 0";
    for (char digit = '0'; digit <= '9'; ++digit)
    {
        line[sizeof(line) - 2] = digit;
        terminal.PrintLn(line);
    }
    terminal.SetStatus("upper", "2/4  Mon Oct 19 9:37 PM");
    terminal.Print("> x");
    terminal.EraseCharacter();
    terminal.Print("y\n> ");

    const auto incremental = pixels;
    size_t lit_pixels = 0;
    for (uint8_t pixel : incremental)
    {
        lit_pixels += pixel != 0;
    }
    REQUIRE(lit_pixels > 0);

    terminal.Redraw();
    REQUIRE(pixels == incremental);
}

TEST_CASE("terminal follows VT100 cursor, erase and reverse-video sequences")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 112;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, width * 4, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));

    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);
    REQUIRE(terminal.ContentRows() == 4);
    REQUIRE(terminal.Columns() == 38);
    const auto row_text = [&](uint32_t row, uint32_t length)
    {
        std::string text;
        for (uint32_t column = 0; column < length; ++column)
        {
            text += static_cast<char>(terminal.CharacterAt(row, column));
        }
        return text;
    };

    terminal.Print("hello\rJ\bX");
    REQUIRE(row_text(1, 6) == "Xello ");

    // Row 1 is the first row below the status bar.
    terminal.Print("\x1b[3;4Habc");
    REQUIRE(row_text(3, 7) == "   abc ");
    REQUIRE(terminal.CursorRow() == 3);
    REQUIRE(terminal.CursorColumn() == 6);
    terminal.Print("\x1b[2D\x1b[A!");
    REQUIRE(row_text(2, 6) == "    ! ");
    terminal.Print("\x1b[99B\x1b[99C");
    REQUIRE(terminal.CursorRow() == 4);
    REQUIRE(terminal.CursorColumn() == 37);
    terminal.Print("\x1b[1;1H\x1b[0;0H");
    REQUIRE(terminal.CursorRow() == 1);
    REQUIRE(terminal.CursorColumn() == 0);

    // A sequence split across writes still applies.
    terminal.Print("\x1b[");
    terminal.Print("3");
    terminal.Print(";2Hq");
    REQUIRE(row_text(3, 2) == " q");

    terminal.Print("\x1b[3;5H\x1b[K");
    REQUIRE(row_text(3, 7) == " q a   ");
    terminal.Print("\x1b[1;3H\x1b[1K");
    REQUIRE(row_text(1, 5) == "   lo");

    terminal.Print("\x1b[2;1Hn\x1b[7mrev\x1b[27mn\x1b[7mx\x1b[0my");
    REQUIRE(row_text(2, 7) == "nrevnxy");
    REQUIRE_FALSE(terminal.ReversedAt(2, 0));
    REQUIRE(terminal.ReversedAt(2, 1));
    REQUIRE(terminal.ReversedAt(2, 3));
    REQUIRE_FALSE(terminal.ReversedAt(2, 4));
    REQUIRE(terminal.ReversedAt(2, 5));
    REQUIRE_FALSE(terminal.ReversedAt(2, 6));

    terminal.Print("\x1b[s\x1b[4;9H\x1b[u");
    REQUIRE(terminal.CursorRow() == 2);
    REQUIRE(terminal.CursorColumn() == 7);
    terminal.Print("\x1b" "7\x1b[1;1H\x1b" "8");
    REQUIRE(terminal.CursorRow() == 2);
    REQUIRE(terminal.CursorColumn() == 7);

    // Unknown sequences are swallowed rather than printed.
    terminal.Print("\x1b[?2004h\x1b[5n.");
    REQUIRE(row_text(2, 8) == "nrevnxy.");

    terminal.SetStatus("edit", "");
    terminal.Print("\x1b[2J");
    for (uint32_t row = 1; row <= 4; ++row)
    {
        REQUIRE(row_text(row, 38) == std::string(38, ' '));
        REQUIRE_FALSE(terminal.ReversedAt(row, 1));
    }
    REQUIRE(row_text(0, 5) == " edit");
}

TEST_CASE("the alternate screen keeps the main screen to come back to")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 112;
    std::array<uint8_t, width * height * 4> pixels{};
    limine_framebuffer descriptor = MakeFramebuffer(pixels.data(), width, height, width * 4, 32);
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));
    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);
    terminal.SetStatus("shell", "");
    const auto row_text = [&](uint32_t row)
    {
        std::string text;
        for (uint32_t column = 0; column < 6; ++column)
        {
            text += static_cast<char>(terminal.CharacterAt(row, column));
        }
        return text;
    };
    terminal.Print("shell\n> vi");
    REQUIRE(terminal.CursorRow() == 2);
    REQUIRE(terminal.CursorColumn() == 4);
    const auto main_screen = pixels;

    terminal.Print("\x1b[?1049h");
    REQUIRE(row_text(1) == "      ");
    REQUIRE(row_text(2) == "      ");
    terminal.Print("\x1b[1;1H\x1b[7meditor\x1b[0m\x1b[4;1H~");
    REQUIRE(row_text(1) == "editor");
    REQUIRE(terminal.ReversedAt(1, 0));
    // Entering again changes nothing, and the status bar belongs to neither screen.
    terminal.Print("\x1b[?1049h");
    REQUIRE(row_text(1) == "editor");
    REQUIRE(row_text(0) == " shell");

    terminal.Print("\x1b[?1049l");
    REQUIRE(row_text(1) == "shell ");
    REQUIRE(row_text(2) == "> vi  ");
    REQUIRE(row_text(4) == "      ");
    REQUIRE_FALSE(terminal.ReversedAt(1, 0));
    REQUIRE(terminal.CursorRow() == 2);
    REQUIRE(terminal.CursorColumn() == 4);
    REQUIRE(pixels == main_screen);

    // A program that exits on the alternate screen is taken off it.
    terminal.Print("\x1b[?1049hgone");
    terminal.ResetModes();
    REQUIRE(row_text(2) == "> vi  ");
}

namespace
{

limine_framebuffer FormatFramebuffer(uint8_t* pixels, uint64_t width, uint64_t height,
                                     const PixelFormat& format)
{
    limine_framebuffer framebuffer{};
    framebuffer.address = pixels;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = width * format.bytes_per_pixel;
    framebuffer.bpp = static_cast<uint16_t>(format.bytes_per_pixel * 8);
    framebuffer.memory_model = LIMINE_FRAMEBUFFER_RGB;
    framebuffer.red_mask_size = format.red_size;
    framebuffer.red_mask_shift = format.red_shift;
    framebuffer.green_mask_size = format.green_size;
    framebuffer.green_mask_shift = format.green_shift;
    framebuffer.blue_mask_size = format.blue_size;
    framebuffer.blue_mask_shift = format.blue_shift;
    return framebuffer;
}

constexpr PixelFormat Xrgb8888 = {4, 8, 16, 8, 8, 8, 0};
constexpr PixelFormat Bgrx8888 = {4, 8, 0, 8, 8, 8, 16};
constexpr PixelFormat Rgb888 = {3, 8, 16, 8, 8, 8, 0};
constexpr PixelFormat Rgb565 = {2, 5, 11, 6, 5, 5, 0};
constexpr PixelFormat AllFormats[] = {Xrgb8888, Bgrx8888, Rgb888, Rgb565};

std::vector<uint8_t> Packed(const PixelFormat& format, std::vector<uint32_t> colors)
{
    std::vector<uint8_t> bytes(colors.size() * format.bytes_per_pixel, 0xEE);
    PackRow(format, colors.data(), bytes.data(), static_cast<uint32_t>(colors.size()));
    return bytes;
}

} // namespace

TEST_CASE("colors convert to each pixel format as computed by hand")
{
    REQUIRE(PackColor(Xrgb8888, 0x00123456) == 0x00123456);
    REQUIRE(PackColor(Bgrx8888, 0x00123456) == 0x00563412);
    REQUIRE(PackColor(Rgb888, 0x00123456) == 0x00123456);
    REQUIRE(PackColor(Rgb565, 0x00123456) == 0x11AA);
    REQUIRE(Packed(Xrgb8888, {0x00123456}) == std::vector<uint8_t>{0x56, 0x34, 0x12, 0x00});
    REQUIRE(Packed(Bgrx8888, {0x00123456}) == std::vector<uint8_t>{0x12, 0x34, 0x56, 0x00});
    REQUIRE(Packed(Rgb888, {0x00123456}) == std::vector<uint8_t>{0x56, 0x34, 0x12});
    REQUIRE(Packed(Rgb565, {0x00123456}) == std::vector<uint8_t>{0xAA, 0x11});
    REQUIRE(Packed(Rgb565, {0x00FFFFFF, 0x00000000}) == std::vector<uint8_t>{0xFF, 0xFF, 0, 0});
}

TEST_CASE("a row, a pixel and a present convert alike in every format")
{
    constexpr uint32_t count = 256;
    std::vector<uint32_t> colors(count);
    uint32_t seed = 12345;
    for (uint32_t& color : colors)
    {
        seed = seed * 1664525 + 1013904223;
        color = seed & 0x00FFFFFF;
    }
    for (const PixelFormat& format : AllFormats)
    {
        INFO("bytes per pixel " << format.bytes_per_pixel << " red shift "
                                << static_cast<int>(format.red_shift));
        std::vector<uint8_t> expected;
        for (const uint32_t color : colors)
        {
            const uint32_t pixel = PackColor(format, color);
            for (uint32_t byte = 0; byte < format.bytes_per_pixel; ++byte)
            {
                expected.push_back(static_cast<uint8_t>(pixel >> (8 * byte)));
            }
        }
        REQUIRE(Packed(format, colors) == expected);

        std::vector<uint8_t> hardware(expected.size());
        limine_framebuffer descriptor = FormatFramebuffer(hardware.data(), count, 1, format);
        auto framebuffer = std::make_unique<Framebuffer>();
        REQUIRE(framebuffer->init(&descriptor));
        for (uint32_t x = 0; x < count; ++x)
        {
            framebuffer->drawPixel(x, 0, colors[x]);
        }
        framebuffer->draw();
        REQUIRE(hardware == expected);

        framebuffer->clear(0xFFFFFF);
        framebuffer->presentRow(0, 0, colors.data(), count);
        framebuffer->draw();
        REQUIRE(hardware == expected);
    }
}

TEST_CASE("only an exact XRGB8888 present passes the high byte on")
{
    const std::vector<uint32_t> colors = {0xFF123456, 0xAB000001};
    REQUIRE(Packed(Bgrx8888, colors) == std::vector<uint8_t>{0x12, 0x34, 0x56, 0x00, 0x00, 0x00,
                                                             0x01, 0x00});
    REQUIRE(Packed(Rgb888, colors) == std::vector<uint8_t>{0x56, 0x34, 0x12, 0x01, 0x00, 0x00});
    // Exact XRGB8888 is copied, so the hardware's unused byte gets it.
    REQUIRE(Packed(Xrgb8888, colors) == std::vector<uint8_t>{0x56, 0x34, 0x12, 0xFF, 0x01, 0x00,
                                                             0x00, 0xAB});
}

TEST_CASE("a present row never writes outside the screen")
{
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 4;
    constexpr uint8_t canary = 0xA5;
    std::vector<uint8_t> hardware(width * height * 4);
    limine_framebuffer descriptor = FormatFramebuffer(hardware.data(), width, height, Xrgb8888);
    auto framebuffer = std::make_unique<Framebuffer>();
    REQUIRE(framebuffer->init(&descriptor));
    std::vector<uint32_t> colors(32);
    for (uint32_t index = 0; index < colors.size(); ++index)
    {
        colors[index] = 0x00010000 * (index + 1);
    }

    struct Case
    {
        int64_t x;
        uint32_t y;
        uint32_t first_column;
        uint32_t drawn;
        uint32_t first_color;
    };
    const Case cases[] = {
        {0, 0, 0, 32, 0},
        {-5, 0, 0, 27, 5},
        {60, 1, 60, 4, 0},
        {64, 2, 0, 0, 0},
        {-32, 3, 0, 0, 0},
        {-31, 3, 0, 1, 31},
        {INT64_MIN, 0, 0, 0, 0},
        {INT64_MAX, 0, 0, 0, 0},
        {0, height, 0, 0, 0},
        {0, UINT32_MAX, 0, 0, 0},
    };
    for (const Case& test : cases)
    {
        INFO("x " << test.x << " y " << test.y);
        framebuffer->clear(0xA5A5A5);
        framebuffer->presentRow(test.x, test.y, colors.data(),
                                static_cast<uint32_t>(colors.size()));
        framebuffer->draw();
        for (uint32_t row = 0; row < height; ++row)
        {
            for (uint32_t column = 0; column < width; ++column)
            {
                const uint8_t* pixel = hardware.data() + (row * width + column) * 4;
                if (row == test.y && column >= test.first_column &&
                    column < test.first_column + test.drawn)
                {
                    const uint32_t color = colors[test.first_color + column - test.first_column];
                    REQUIRE(pixel[2] == ((color >> 16) & 0xFF));
                    REQUIRE(pixel[0] == 0);
                }
                else
                {
                    REQUIRE(pixel[0] == canary);
                    REQUIRE(pixel[1] == canary);
                    REQUIRE(pixel[2] == canary);
                    REQUIRE(pixel[3] == 0);
                }
            }
        }
    }
}

namespace
{

struct TestScreen
{
    static constexpr uint64_t Width = 320;
    static constexpr uint64_t Height = 112;

    TestScreen()
    {
        descriptor = MakeFramebuffer(pixels.data(), Width, Height, Width * 4, 32);
        REQUIRE(framebuffer.init(&descriptor));
    }

    std::array<uint8_t, Width * Height * 4> pixels{};
    limine_framebuffer descriptor{};
    Framebuffer framebuffer;
};

bool AllBlack(const TestScreen& screen)
{
    return std::all_of(screen.pixels.begin(), screen.pixels.end(),
                       [](uint8_t byte) { return byte == 0; });
}

void FeedConsole(FeatherOS::Term& terminal)
{
    terminal.SetStatus("shell", "1/4  Mon Oct 19 9:36 PM");
    terminal.Print("> ");
    terminal.SetCursorVisible(true);
    for (int line = 0; line < 200; ++line)
    {
        terminal.PrintLn(("line " + std::to_string(line)).c_str());
    }
    terminal.SetStatus("upper", "2/4  Mon Oct 19 9:37 PM");
    terminal.Print("done");
}

} // namespace

TEST_CASE("a terminal in graphics mode takes text and draws nothing")
{
    auto screen = std::make_unique<TestScreen>();
    FeatherOS::Term terminal;
    terminal.Init(&screen->framebuffer);
    terminal.SetStatus("shell", "1/4");
    terminal.Print("before\n> ");
    REQUIRE_FALSE(AllBlack(*screen));

    terminal.SetGraphics(true);
    REQUIRE(terminal.Graphics());
    REQUIRE(AllBlack(*screen));
    FeedConsole(terminal);
    REQUIRE(AllBlack(*screen));
    REQUIRE(terminal.CharacterAt(0, 1) == 'u');

    terminal.SetGraphics(false);
    REQUIRE_FALSE(terminal.Graphics());
    auto reference_screen = std::make_unique<TestScreen>();
    FeatherOS::Term reference;
    reference.Init(&reference_screen->framebuffer);
    reference.SetStatus("shell", "1/4");
    reference.Print("before\n> ");
    FeedConsole(reference);
    REQUIRE_FALSE(AllBlack(*screen));
    REQUIRE(screen->pixels == reference_screen->pixels);
}

TEST_CASE("a hidden terminal in graphics mode blanks the screen when shown")
{
    auto screen = std::make_unique<TestScreen>();
    FeatherOS::Term first;
    FeatherOS::Term second;
    first.Init(&screen->framebuffer);
    second.Init(&screen->framebuffer);
    second.SetShown(false);
    first.SetStatus("first", "");
    first.Print("first console");
    const auto first_pixels = screen->pixels;

    second.SetGraphics(true);
    second.Print("second");
    second.SetStatus("second", "");
    REQUIRE(screen->pixels == first_pixels);

    first.SetShown(false);
    second.SetShown(true);
    REQUIRE(AllBlack(*screen));
    second.Print(" more");
    REQUIRE(AllBlack(*screen));

    second.SetShown(false);
    first.SetShown(true);
    REQUIRE(screen->pixels == first_pixels);
    first.SetShown(false);
    second.SetShown(true);
    REQUIRE(AllBlack(*screen));

    second.SetGraphics(false);
    REQUIRE_FALSE(AllBlack(*screen));
    auto reference_screen = std::make_unique<TestScreen>();
    FeatherOS::Term reference;
    reference.Init(&reference_screen->framebuffer);
    reference.SetStatus("second", "");
    reference.Print("second more");
    REQUIRE(screen->pixels == reference_screen->pixels);
}

TEST_CASE("a hidden terminal leaving graphics mode waits to be shown")
{
    auto screen = std::make_unique<TestScreen>();
    FeatherOS::Term first;
    FeatherOS::Term second;
    first.Init(&screen->framebuffer);
    second.Init(&screen->framebuffer);
    second.SetGraphics(true);
    second.SetShown(false);
    first.SetShown(true);
    first.Print("text");
    const auto first_pixels = screen->pixels;
    second.SetGraphics(false);
    REQUIRE(screen->pixels == first_pixels);
    first.SetShown(false);
    second.SetShown(true);
    REQUIRE_FALSE(screen->pixels == first_pixels);
}

namespace
{

namespace Text = FeatherOS::Text;
using FeatherOS::TermConfig;

constexpr uint32_t CellWidth = TermConfig::CellWidth;
constexpr uint32_t CellHeight = TermConfig::CellHeight;
constexpr uint32_t White = 0xFFFFFF;
constexpr uint32_t Black = 0x000000;

Text::FontView ConsoleView()
{
    Text::FontView view;
    REQUIRE(Text::FontView::Parse(ConsoleFontData, ConsoleFontSize, &view) == Text::FontStatus::Ok);
    return view;
}

std::array<uint32_t, CellWidth * CellHeight> ReferenceCell(const Text::FontView& view,
                                                           char32_t code_point,
                                                           uint32_t background,
                                                           uint32_t foreground)
{
    std::array<uint32_t, CellWidth * CellHeight> cell;
    cell.fill(background);
    const Text::Glyph glyph = view.Find(code_point);
    for (int32_t row = 0; row < glyph.height; ++row)
    {
        for (int32_t column = 0; column < glyph.width; ++column)
        {
            const int32_t x = glyph.left + column;
            const int32_t y = view.Ascent() - glyph.top + row;
            const uint8_t coverage = view.Coverage(glyph, column, row);
            if (x >= 0 && x < static_cast<int32_t>(CellWidth) && y >= 0 &&
                y < static_cast<int32_t>(CellHeight) && coverage != 0)
            {
                cell[static_cast<size_t>(y) * CellWidth + static_cast<size_t>(x)] =
                    Text::Blend(background, foreground, coverage);
            }
        }
    }
    return cell;
}

std::array<uint32_t, CellWidth * CellHeight> CellAt(const TestScreen& screen, uint32_t row,
                                                    uint32_t column)
{
    std::array<uint32_t, CellWidth * CellHeight> cell;
    const uint32_t x0 = (column + TermConfig::Padding) * CellWidth;
    const uint32_t y0 = (row + TermConfig::Padding) * CellHeight;
    for (uint32_t y = 0; y < CellHeight; ++y)
    {
        for (uint32_t x = 0; x < CellWidth; ++x)
        {
            const size_t offset = ((y0 + y) * TestScreen::Width + x0 + x) * 4;
            cell[y * CellWidth + x] = screen.pixels[offset] | (screen.pixels[offset + 1] << 8) |
                                      (screen.pixels[offset + 2] << 16);
        }
    }
    return cell;
}

std::vector<char32_t> RowCodePoints(const FeatherOS::Term& terminal, uint32_t row, uint32_t length)
{
    std::vector<char32_t> result;
    for (uint32_t column = 0; column < length; ++column)
    {
        result.push_back(terminal.CharacterAt(row, column));
    }
    return result;
}

bool HasInk(const std::array<uint32_t, CellWidth * CellHeight>& cell, uint32_t background)
{
    return std::any_of(cell.begin(), cell.end(),
                       [background](uint32_t pixel) { return pixel != background; });
}

} // namespace

TEST_CASE("the compiled-in console font parses and fits the console cell")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    REQUIRE(FeatherOS::Term::FontStatus() == Text::FontStatus::Ok);
    const Text::FontView view = ConsoleView();
    REQUIRE(view.Monospace());
    REQUIRE(view.CellWidth() == CellWidth);
    REQUIRE(view.LineHeight() == CellHeight);
    REQUIRE(std::string(view.Name()) == "mono 16");
    for (char32_t code_point = 0x20; code_point < 0x7F; ++code_point)
    {
        REQUIRE(view.Contains(code_point));
    }
    REQUIRE(TermConfig::Row == 48);
    REQUIRE(TermConfig::Column == 128);
}

TEST_CASE("a terminal draws every glyph of the console font as the reference does")
{
    const Text::FontView view = ConsoleView();
    for (const PixelFormat& format : AllFormats)
    {
        INFO("bytes per pixel " << format.bytes_per_pixel);
        constexpr uint32_t width = 3 * CellWidth;
        constexpr uint32_t height = 3 * CellHeight;
        std::vector<uint8_t> hardware(width * height * format.bytes_per_pixel);
        limine_framebuffer descriptor = FormatFramebuffer(hardware.data(), width, height, format);
        auto framebuffer = std::make_unique<Framebuffer>();
        REQUIRE(framebuffer->init(&descriptor));
        uint32_t palette[16];
        for (uint32_t coverage = 0; coverage < 16; ++coverage)
        {
            palette[coverage] = framebuffer->packColor(Text::Blend(Black, White, coverage));
        }
        for (uint32_t index = 0; index < view.GlyphCount(); ++index)
        {
            const Text::Glyph glyph = view.GlyphAt(index);
            INFO("code point " << static_cast<uint32_t>(glyph.code_point));
            framebuffer->clear(Black);
            framebuffer->drawGlyph(view, glyph, CellWidth, CellHeight, palette);
            framebuffer->draw();
            const auto expected = ReferenceCell(view, glyph.code_point, Black, White);
            for (uint32_t y = 0; y < height; ++y)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    const bool inside = x >= CellWidth && x < 2 * CellWidth && y >= CellHeight &&
                                        y < 2 * CellHeight;
                    const uint32_t color =
                        inside ? expected[(y - CellHeight) * CellWidth + x - CellWidth] : Black;
                    const auto want = Packed(format, {color});
                    const auto offset = (y * width + x) * format.bytes_per_pixel;
                    REQUIRE(std::equal(want.begin(), want.end(), hardware.begin() + offset));
                }
            }
        }
    }
}

TEST_CASE("a glyph at the screen's edge is clipped to the screen")
{
    const Text::FontView view = ConsoleView();
    TestScreen screen;
    uint32_t palette[16];
    for (uint32_t coverage = 0; coverage < 16; ++coverage)
    {
        palette[coverage] = screen.framebuffer.packColor(Text::Blend(Black, White, coverage));
    }
    const Text::Glyph block = view.Find(0x2588);
    screen.framebuffer.drawGlyph(view, block, TestScreen::Width - 3, TestScreen::Height - 5,
                                 palette);
    screen.framebuffer.drawGlyph(view, block, TestScreen::Width, 0, palette);
    screen.framebuffer.drawGlyph(view, block, 0, TestScreen::Height, palette);
    screen.framebuffer.draw();
    size_t lit = 0;
    for (size_t offset = 0; offset < screen.pixels.size(); offset += 4)
    {
        lit += screen.pixels[offset] != 0;
    }
    REQUIRE(lit == 15);
}

TEST_CASE("a terminal draws a known glyph into its cell")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    const Text::FontView view = ConsoleView();
    terminal.Print("A\xC3\xA9\xE2\x96\x88");
    const auto a = CellAt(screen, 1, 0);
    REQUIRE(a == ReferenceCell(view, 'A', Black, White));
    REQUIRE(HasInk(a, Black));
    REQUIRE(std::count(a.begin(), a.end(), White) > 0);
    REQUIRE(std::any_of(a.begin(), a.end(),
                        [](uint32_t pixel) { return pixel != Black && pixel != White; }));
    REQUIRE(CellAt(screen, 1, 1) == ReferenceCell(view, 0xE9, Black, White));
    const auto block = CellAt(screen, 1, 2);
    REQUIRE(std::all_of(block.begin(), block.end(), [](uint32_t pixel) { return pixel == White; }));
    REQUIRE(CellAt(screen, 1, 5) == ReferenceCell(view, ' ', Black, White));
}

TEST_CASE("a code point the font lacks draws the missing box and a control draws nothing")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    const Text::FontView view = ConsoleView();
    REQUIRE_FALSE(view.Contains(0x4E2D));
    REQUIRE_FALSE(view.Contains(0x132));
    terminal.Print("\xE4\xB8\xAD\xC4\xB2\t");
    const auto missing = ReferenceCell(view, 0x4E2D, Black, White);
    REQUIRE(HasInk(missing, Black));
    REQUIRE(CellAt(screen, 1, 0) == missing);
    REQUIRE(CellAt(screen, 1, 1) == missing);
    REQUIRE(terminal.CharacterAt(1, 0) == 0x4E2D);
    REQUIRE(terminal.CharacterAt(1, 2) == '\t');
    REQUIRE_FALSE(HasInk(CellAt(screen, 1, 2), Black));
}

TEST_CASE("UTF-8 split over Print calls fills one cell and each terminal keeps its own state")
{
    TestScreen screen;
    FeatherOS::Term first;
    FeatherOS::Term second;
    first.Init(&screen.framebuffer);
    second.Init(&screen.framebuffer);
    second.SetShown(false);

    first.Print("caf\xC3");
    second.Print("\xE2\x86");
    first.Print("\xA9");
    second.Print("ab");
    REQUIRE(RowCodePoints(first, 1, 5) == std::vector<char32_t>{'c', 'a', 'f', 0xE9, ' '});
    REQUIRE(first.CursorColumn() == 4);
    REQUIRE(RowCodePoints(second, 1, 3) == std::vector<char32_t>{0xFFFD, 'a', 'b'});
    REQUIRE(second.CursorColumn() == 3);

    first.Print("\xE2");
    second.Print("\xE2\x86");
    first.Print("\x86");
    second.Print("\x92");
    first.Print("\x92");
    REQUIRE(first.CharacterAt(1, 4) == 0x2192);
    REQUIRE(second.CharacterAt(1, 3) == 0x2192);

    first.Print("\xF0\x9F");
    first.Print("\x98");
    first.Print("\x80");
    REQUIRE(first.CharacterAt(1, 5) == 0x1F600);
    REQUIRE(first.CursorColumn() == 6);

    first.Print("\xC3");
    first.Init(&screen.framebuffer);
    first.Print("x");
    REQUIRE(first.CharacterAt(1, 0) == 'x');
}

TEST_CASE("invalid UTF-8 becomes replacement cells and C1 controls are dropped")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    const auto row = [&](uint32_t length) { return RowCodePoints(terminal, 1, length); };

    terminal.Print("\xFF\x80\xC0\x80z");
    REQUIRE(row(6) == std::vector<char32_t>{0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 'z', ' '});

    terminal.Print("\x1b[1;1H\x1b[2K");
    terminal.Print("\xE2\x82");
    terminal.Print("x");
    REQUIRE(row(3) == std::vector<char32_t>{0xFFFD, 'x', ' '});

    terminal.Print("\x1b[1;1H\x1b[2K");
    terminal.Print("\xED\xA0\x80\xF4\x90\x80\x80");
    REQUIRE(row(8) == std::vector<char32_t>{0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
                                            ' '});

    terminal.Print("\x1b[1;1H\x1b[2K");
    terminal.Print("a\xC2\x85\xC2\x9F" "b\xC2\xA0");
    REQUIRE(row(4) == std::vector<char32_t>{'a', 'b', 0xA0, ' '});

    terminal.Print("\x1b[1;1H\x1b[2K");
    terminal.Print("\xC3\x1b[7mR\x1b[0m");
    REQUIRE(row(3) == std::vector<char32_t>{0xFFFD, 'R', ' '});
    REQUIRE_FALSE(terminal.ReversedAt(1, 0));
    REQUIRE(terminal.ReversedAt(1, 1));

    terminal.Print("\x1b[1;1H\x1b[2K");
    terminal.Print("\xE2\x86\nq");
    REQUIRE(RowCodePoints(terminal, 1, 2) == std::vector<char32_t>{0xFFFD, ' '});
    REQUIRE(terminal.CharacterAt(2, 0) == 'q');

    terminal.Print("\x1b[1;1H\x1b[2K");
    terminal.Print("\xF0\x9F\x1b" "7\x1b[3;1H");
    REQUIRE(terminal.CharacterAt(1, 0) == 0xFFFD);
    REQUIRE(terminal.CursorRow() == 3);

    terminal.Print("\x1b[?25\xC3\xA9l.");
    REQUIRE(terminal.CharacterAt(3, 0) == '.');
}

TEST_CASE("a code point above U+00FF keeps its bits and the attributes stay separate")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    terminal.Print("\x1b[7m\xF4\x8F\xBF\xBF\xE2\x96\x88\x1b[0m\xE2\x96\x88");
    REQUIRE(terminal.CharacterAt(1, 0) == 0x10FFFF);
    REQUIRE(terminal.CharacterAt(1, 1) == 0x2588);
    REQUIRE(terminal.CharacterAt(1, 2) == 0x2588);
    REQUIRE(terminal.ReversedAt(1, 0));
    REQUIRE(terminal.ReversedAt(1, 1));
    REQUIRE_FALSE(terminal.ReversedAt(1, 2));
    REQUIRE(terminal.CharacterAt(1, 3) == ' ');
    REQUIRE_FALSE(terminal.ReversedAt(1, 3));
}

TEST_CASE("reverse video, the cursor and the status bar use the inverted palette")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    const Text::FontView view = ConsoleView();
    terminal.SetStatus("g\xC3\xA9", "");
    terminal.Print("\x1b[7mgW\x1b[0m\xE2\x96\x88Q");
    terminal.SetCursorVisible(true);

    REQUIRE(CellAt(screen, 1, 0) == ReferenceCell(view, 'g', White, Black));
    REQUIRE(CellAt(screen, 1, 1) == ReferenceCell(view, 'W', White, Black));
    REQUIRE(CellAt(screen, 1, 2) == ReferenceCell(view, 0x2588, Black, White));
    REQUIRE(CellAt(screen, 1, 3) == ReferenceCell(view, 'Q', Black, White));
    // The cursor rests on the blank cell after the text, the opposite of a normal cell.
    const auto cursor = CellAt(screen, 1, 4);
    REQUIRE(std::all_of(cursor.begin(), cursor.end(),
                        [](uint32_t pixel) { return pixel == White; }));
    REQUIRE(CellAt(screen, 0, 1) == ReferenceCell(view, 'g', White, Black));
    REQUIRE(CellAt(screen, 0, 2) == ReferenceCell(view, 0xE9, White, Black));
    REQUIRE(CellAt(screen, 0, 5) == ReferenceCell(view, ' ', White, Black));

    terminal.Print("\x1b[1;1H\x1b[7m");
    terminal.SetCursorVisible(false);
    terminal.SetCursorVisible(true);
    // A reversed cell under the cursor shows in normal colors.
    REQUIRE(CellAt(screen, 1, 0) == ReferenceCell(view, 'g', Black, White));
}

TEST_CASE("status text is decoded into cells, aligned by character and cut by cells")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    const auto bar = [&](uint32_t length) { return RowCodePoints(terminal, 0, length); };
    terminal.SetStatus("\xC3\xA9t\xC3", "\xCE\xB1\xCE\xB2\xC2\x85\xFF");
    std::vector<char32_t> expected(38, ' ');
    expected[1] = 0xE9;
    expected[2] = 't';
    expected[3] = 0xFFFD;
    expected[34] = 0x3B1;
    expected[35] = 0x3B2;
    expected[36] = 0xFFFD;
    REQUIRE(bar(38) == expected);
    terminal.SetStatus("\xE2\x86\x92" "ab", "");
    REQUIRE(RowCodePoints(terminal, 0, 5) == std::vector<char32_t>{' ', 0x2192, 'a', 'b', ' '});
    terminal.SetStatus("", "x\xE2\x86");
    REQUIRE(terminal.CharacterAt(0, 35) == 'x');
    REQUIRE(terminal.CharacterAt(0, 36) == 0xFFFD);
}

TEST_CASE("scrolling and the alternate screen keep whole code points")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    terminal.Print("\xCE\xB1\n\xCE\xB2\xE2\x86\x92\n\xD0\x96\n\xE2\x95\x94\xE2\x95\x90\nlast\n\xC3\xA9");
    REQUIRE(terminal.CharacterAt(1, 0) == 0x416);
    REQUIRE(RowCodePoints(terminal, 2, 2) == std::vector<char32_t>{0x2554, 0x2550});
    REQUIRE(terminal.CharacterAt(3, 0) == 'l');
    REQUIRE(terminal.CharacterAt(4, 0) == 0xE9);
    const auto scrolled = screen.pixels;

    terminal.Print("\x1b[?1049h\x1b[1;1H\xD0\x96\xD0\x96");
    REQUIRE(terminal.CharacterAt(1, 0) == 0x416);
    REQUIRE(terminal.CharacterAt(4, 0) == ' ');
    terminal.Print("\x1b[?1049l");
    REQUIRE(terminal.CharacterAt(1, 0) == 0x416);
    REQUIRE(RowCodePoints(terminal, 2, 2) == std::vector<char32_t>{0x2554, 0x2550});
    REQUIRE(terminal.CharacterAt(3, 0) == 'l');
    REQUIRE(terminal.CharacterAt(4, 0) == 0xE9);
    REQUIRE(screen.pixels == scrolled);

    terminal.Redraw();
    REQUIRE(screen.pixels == scrolled);
}

TEST_CASE("incremental rendering of wide code points matches a full redraw")
{
    TestScreen screen;
    FeatherOS::Term terminal;
    terminal.Init(&screen.framebuffer);
    terminal.SetStatus("\xCE\xB1\xCE\xB2", "\xD0\x96");
    terminal.SetCursorVisible(true);
    for (int line = 0; line < 12; ++line)
    {
        terminal.PrintLn("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90 \xC3\xA1\xC5\x91 \xCE\xA9\xD0\xAF");
    }
    terminal.Print("\x1b[7m\xE2\x86\x92\x1b[0m");
    const auto incremental = screen.pixels;
    terminal.Redraw();
    REQUIRE(screen.pixels == incremental);
}
