#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"

extern "C"
{
#include "kernel/framebuffer/font.h"
}

#include <array>
#include <string>

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
