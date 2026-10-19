#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"

#include <array>

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

TEST_CASE("terminal rendering keeps padding around logs and the prompt row")
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
    terminal.Print("Boot status");
    terminal.BeginInput("FeatherOS> ");
    terminal.SetUptimeText("Uptime: 1s");
    terminal.HandleInputEvent({FeatherOS::Input::EventType::Character, 'x'});
    terminal.HandleInputEvent({FeatherOS::Input::EventType::Backspace, '\0'});

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
    terminal.BeginInput("FeatherOS> ");
    // The burst scrolls past every content row; later single-line scrolls shift existing pixels.
    terminal.Print("a\nb\nc\nd\ne\nf\ng\nh\n");
    char line[] = "log line 0";
    for (char digit = '0'; digit <= '9'; ++digit)
    {
        line[sizeof(line) - 2] = digit;
        terminal.PrintLn(line);
    }
    terminal.SetUptimeText("Uptime: 9s");
    terminal.HandleInputEvent({FeatherOS::Input::EventType::Character, 'x'});
    terminal.HandleInputEvent({FeatherOS::Input::EventType::Enter, '\0'});
    terminal.HandleInputEvent({FeatherOS::Input::EventType::Character, 'y'});

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
