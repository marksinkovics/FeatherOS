#include <catch2/catch_test_macros.hpp>

#include "user/graphics/canvas.h"
#include "user/graphics/surface.h"
#include "user/ui/art.h"
#include "user/ui/card.h"

#include <cstdint>
#include <set>
#include <string>

namespace Graphics = FeatherOS::Graphics;
namespace UI = FeatherOS::UI;

namespace
{

const std::string FontDirectory = std::string(FEATHEROS_FONT_DIRECTORY) + "/";

Graphics::Surface Render(int32_t width, int32_t height, uint32_t page, UI::ThemeId theme,
                         UI::AccentId accent)
{
    REQUIRE(UI::LoadArt(FEATHEROS_OUTLINES_FILE) == Graphics::OutlineStatus::Ok);
    UI::SetCardFontDirectory(FontDirectory.c_str());
    Graphics::Surface surface(width, height);
    Graphics::Canvas canvas(surface);
    UI::DrawCard(canvas, width, height, page, theme, accent);
    return surface;
}

} // namespace

TEST_CASE("every card page draws something of its own, the same each time", "[card]")
{
    std::set<uint32_t> checksums;
    for (uint32_t page = 1; page <= UI::CardPageCount; ++page)
    {
        for (const UI::ThemeId theme : {UI::ThemeId::Dusk, UI::ThemeId::ContrastLight})
        {
            INFO("page " << page);
            const Graphics::Surface first = Render(1024, 768, page, theme, UI::AccentId::Default);
            const Graphics::Surface second = Render(1024, 768, page, theme, UI::AccentId::Default);
            CHECK(first.Checksum() == second.Checksum());
            CHECK(checksums.insert(first.Checksum()).second);
        }
    }
}

TEST_CASE("a card page changes with the accent", "[card]")
{
    for (uint32_t page = 1; page <= 3; ++page)
    {
        INFO("page " << page);
        CHECK(Render(1024, 768, page, UI::ThemeId::Dusk, UI::AccentId::Default).Checksum() !=
              Render(1024, 768, page, UI::ThemeId::Dusk, UI::AccentId::Coral).Checksum());
    }
}

TEST_CASE("a page out of range draws nothing, and any size is safe", "[card]")
{
    Graphics::Surface blank(64, 64);
    Graphics::Surface zero = Render(64, 64, 0, UI::ThemeId::Dusk, UI::AccentId::Default);
    CHECK(zero.Checksum() == blank.Checksum());
    CHECK(Render(64, 64, 5, UI::ThemeId::Dusk, UI::AccentId::Default).Checksum() ==
          blank.Checksum());
    for (uint32_t page = 1; page <= UI::CardPageCount; ++page)
    {
        Render(1, 1, page, UI::ThemeId::Ink, UI::AccentId::Violet);
        Render(200, 120, page, UI::ThemeId::Ink, UI::AccentId::Violet);
        Render(1920, 1080, page, UI::ThemeId::Paper, UI::AccentId::Teal);
    }
}

TEST_CASE("theme and accent ids parse and round-trip", "[card]")
{
    for (uint32_t index = 0; index < UI::ThemeCount; ++index)
    {
        UI::ThemeId parsed = UI::ThemeId::Dusk;
        const auto id = static_cast<UI::ThemeId>(index);
        REQUIRE(UI::ParseThemeId(UI::ThemeIdName(id), &parsed));
        CHECK(parsed == id);
    }
    for (uint32_t index = 0; index < UI::AccentCount; ++index)
    {
        UI::AccentId parsed = UI::AccentId::Default;
        const auto id = static_cast<UI::AccentId>(index);
        REQUIRE(UI::ParseAccentId(UI::AccentIdName(id), &parsed));
        CHECK(parsed == id);
    }
    UI::ThemeId theme = UI::ThemeId::Dusk;
    UI::AccentId accent = UI::AccentId::Default;
    CHECK_FALSE(UI::ParseThemeId("nope", &theme));
    CHECK_FALSE(UI::ParseThemeId(nullptr, &theme));
    CHECK_FALSE(UI::ParseAccentId("", &accent));
}
