#include <catch2/catch_test_macros.hpp>

#include "user/graphics/blend.h"
#include "user/graphics/color.h"

#include <cstdint>
#include <random>

namespace Graphics = FeatherOS::Graphics;
using Graphics::Color;
using Graphics::Div255;

namespace
{

// round(n / 255) for an integer n, as one rational: no value of n lies on a tie.
uint32_t Round255(uint32_t n)
{
    return (2 * n + 255) / 510;
}

uint32_t ReferenceMix(uint32_t s, uint32_t d, uint32_t a)
{
    return Round255(s * a + d * (255 - a));
}

uint32_t ReferenceOver(uint32_t s, uint32_t d, uint32_t a)
{
    const uint32_t result = s + Round255(d * (255 - a));
    return result > 255 ? 255 : result;
}

bool PremultipliedValid(uint32_t pixel)
{
    const uint32_t alpha = pixel >> 24;
    return ((pixel >> 16) & 0xFF) <= alpha && ((pixel >> 8) & 0xFF) <= alpha &&
           (pixel & 0xFF) <= alpha;
}

uint32_t RandomPremultiplied(std::mt19937& random)
{
    const uint32_t shape = random() % 8;
    const uint32_t alpha = shape == 0 ? 0 : shape == 1 ? 255 : random() % 256;
    auto channel = [&]() { return random() % (alpha + 1); };
    return alpha << 24 | channel() << 16 | channel() << 8 | channel();
}

} // namespace

TEST_CASE("Div255 equals the rounded quotient for every product of two bytes", "[blend]")
{
    for (uint32_t x = 0; x <= 65025; ++x)
    {
        INFO("x = " << x);
        REQUIRE(Div255(x) == Round255(x));
    }
    REQUIRE(Div255(0) == 0);
    REQUIRE(Div255(127) == 0);
    REQUIRE(Div255(128) == 1);
    REQUIRE(Div255(65025) == 255);
}

TEST_CASE("The straight rule matches a rational reference for all 2^24 combinations",
          "[blend]")
{
    uint32_t wrong = 0;
    for (uint32_t s = 0; s < 256; ++s)
    {
        for (uint32_t d = 0; d < 256; ++d)
        {
            for (uint32_t a = 0; a < 256; ++a)
            {
                wrong += Graphics::MixChannel(s, d, a) != ReferenceMix(s, d, a) ? 1U : 0U;
            }
        }
    }
    REQUIRE(wrong == 0);
}

TEST_CASE("The premultiplied rule matches a rational reference for all 2^24 combinations",
          "[blend]")
{
    uint32_t wrong = 0;
    for (uint32_t s = 0; s < 256; ++s)
    {
        for (uint32_t d = 0; d < 256; ++d)
        {
            for (uint32_t a = 0; a < 256; ++a)
            {
                wrong += Graphics::OverChannel(s, d, a) != ReferenceOver(s, d, a) ? 1U : 0U;
            }
        }
    }
    REQUIRE(wrong == 0);
}

TEST_CASE("The straight and premultiplied rules agree at the ends of the alpha range", "[blend]")
{
    for (uint32_t s = 0; s < 256; ++s)
    {
        for (uint32_t d = 0; d < 256; ++d)
        {
            REQUIRE(Graphics::MixChannel(s, d, 0) == d);
            REQUIRE(Graphics::MixChannel(s, d, 255) == s);
            REQUIRE(Graphics::OverChannel(0, d, 0) == d);
            REQUIRE(Graphics::OverChannel(s, d, 255) == s);
        }
    }
}

TEST_CASE("Premultiply scales the channels and keeps the alpha", "[blend]")
{
    REQUIRE(Graphics::Premultiply(0xFF336699) == 0xFF336699);
    REQUIRE(Graphics::Premultiply(0x00FFFFFF) == 0x00000000);
    REQUIRE(Graphics::Premultiply(0x80FF8000) == 0x80804000);
    REQUIRE(Graphics::Premultiply(Graphics::WithAlpha(Graphics::Rgb(10, 20, 30), 0x40)) ==
            0x40030508);
}

TEST_CASE("Colour helpers build and change a straight ARGB value", "[blend]")
{
    REQUIRE(Graphics::Rgb(1, 2, 3) == 0xFF010203);
    REQUIRE(Graphics::WithAlpha(Graphics::Rgb(1, 2, 3), 0x7F) == 0x7F010203);
    REQUIRE(Graphics::WithAlpha(0xFF010203, 0x1FF) == 0xFF010203);
}

TEST_CASE("A premultiplied pixel never has a colour channel above its alpha", "[blend]")
{
    std::mt19937 random(37);
    for (int round = 0; round < 10000; ++round)
    {
        const uint32_t destination = RandomPremultiplied(random);
        const uint32_t source = RandomPremultiplied(random);
        const uint32_t opacity = random() % 256;
        REQUIRE(PremultipliedValid(Graphics::ScalePixel(source, opacity)));

        const uint32_t over = Graphics::OverArgb(destination, source);
        REQUIRE(PremultipliedValid(over));
        REQUIRE((over >> 24) >= (source >> 24));
        REQUIRE((over >> 24) >= (destination >> 24));

        const uint32_t faded = Graphics::OverArgb(destination, Graphics::ScalePixel(source, opacity));
        REQUIRE(PremultipliedValid(faded));

        const Color straight = random();
        REQUIRE(PremultipliedValid(Graphics::Premultiply(straight)));
        REQUIRE(PremultipliedValid(Graphics::OverArgb(destination, Graphics::Premultiply(straight))));
    }
}

TEST_CASE("Over onto XRGB keeps the top byte 0 and equals the colour channels of OverArgb",
          "[blend]")
{
    std::mt19937 random(38);
    for (int round = 0; round < 10000; ++round)
    {
        const uint32_t destination = random() & 0xFFFFFF;
        const uint32_t source = RandomPremultiplied(random);
        const uint32_t result = Graphics::OverXrgb(destination, source);
        REQUIRE((result >> 24) == 0);
        REQUIRE(result == (Graphics::OverArgb(destination | 0xFF000000U, source) & 0xFFFFFF));
    }
}
