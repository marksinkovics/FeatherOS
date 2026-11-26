#include <catch2/catch_test_macros.hpp>

#include "user/ui/tokens.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace UI = FeatherOS::UI;
using UI::AccentId;
using UI::Family;
using UI::Mode;
using UI::ThemeId;

namespace
{

struct AccentCase
{
    AccentId id;
    uint32_t accent;
    uint32_t on_accent;
    uint32_t soft;
};

constexpr std::array<AccentCase, 7> OnDusk = {{
    {AccentId::Blue, 0xFF6CB6F0, 0xFF06223A, 0xFF385574},
    {AccentId::Teal, 0xFF4FC4B0, 0xFF04261F, 0xFF2F5A61},
    {AccentId::Green, 0xFF8CC66A, 0xFF142407, 0xFF415A4C},
    {AccentId::Amber, 0xFFF0A840, 0xFF2A1A04, 0xFF5F513F},
    {AccentId::Coral, 0xFFFF7A4D, 0xFF2A0C04, 0xFF644343},
    {AccentId::Pink, 0xFFF07AA8, 0xFF2E0716, 0xFF5F435F},
    {AccentId::Violet, 0xFFA98CF0, 0xFF1A0F36, 0xFF4A4974},
}};

constexpr std::array<AccentCase, 7> OnAiry = {{
    {AccentId::Blue, 0xFF1565B0, 0xFFFFFFFF, 0xFFD2E1EF},
    {AccentId::Teal, 0xFF0F7364, 0xFFFFFFFF, 0xFFD1E3E1},
    {AccentId::Green, 0xFF3B7326, 0xFFFFFFFF, 0xFFD8E3D6},
    {AccentId::Amber, 0xFF945800, 0xFFFFFFFF, 0xFFE8DECF},
    {AccentId::Coral, 0xFFB3452A, 0xFFFFFFFF, 0xFFEEDBD7},
    {AccentId::Pink, 0xFFAD2F66, 0xFFFFFFFF, 0xFFEDD7E2},
    {AccentId::Violet, 0xFF6644C0, 0xFFFFFFFF, 0xFFE0DBF2},
}};

} // namespace

TEST_CASE("BaseTheme returns the values of Calm Dusk", "[tokens]")
{
    const UI::Theme& theme = UI::BaseTheme(ThemeId::Dusk);
    CHECK(theme.id == ThemeId::Dusk);
    CHECK(theme.family == Family::Dusk);
    CHECK(theme.dark);
    CHECK(std::strcmp(theme.name, "Calm Dusk") == 0);
    CHECK(theme.desk == 0xFF141C2B);
    CHECK(theme.bar == 0xFF1A2334);
    CHECK(theme.surface == 0xFF212C3F);
    CHECK(theme.raised == 0xFF2A374C);
    CHECK(theme.sunken == 0xFF182132);
    CHECK(theme.line == 0xFF3A4860);
    CHECK(theme.text == 0xFFE8EDF5);
    CHECK(theme.muted == 0xFFA7B3C6);
    CHECK(theme.accent == 0xFFF0A840);
    CHECK(theme.on_accent == 0xFF2A1A04);
    CHECK(theme.accent_soft == 0xFF4A3B28);
    CHECK(theme.ok == 0xFF62C48F);
    CHECK(theme.warn == 0xFFF0C04A);
    CHECK(theme.bad == 0xFFF0776C);
    CHECK(theme.info == 0xFF6CB6F0);
    CHECK(theme.focus == 0xFFFFD28A);
    CHECK(theme.wallpaper_1 == 0xFF1A2740);
    CHECK(theme.wallpaper_2 == 0xFF22324D);
    CHECK(theme.wallpaper_3 == 0xFF2D3F5A);
    CHECK(theme.wallpaper_feather == 0x26F0A840);
    CHECK(theme.shadow == 0x8C040810);
}

TEST_CASE("BaseTheme returns the values of High Contrast Light", "[tokens]")
{
    const UI::Theme& theme = UI::BaseTheme(ThemeId::ContrastLight);
    CHECK(theme.id == ThemeId::ContrastLight);
    CHECK(theme.family == Family::HighContrast);
    CHECK_FALSE(theme.dark);
    CHECK(theme.desk == 0xFFFFFFFF);
    CHECK(theme.sunken == 0xFFF2F2F2);
    CHECK(theme.line == 0xFF000000);
    CHECK(theme.text == 0xFF000000);
    CHECK(theme.muted == 0xFF262626);
    CHECK(theme.accent == 0xFF0040C0);
    CHECK(theme.accent_soft == 0xFFD6E2FF);
    CHECK(theme.warn == 0xFF7A4D00);
    CHECK(theme.bad == 0xFFB00020);
    CHECK(theme.focus == 0xFFC00060);
    CHECK(theme.wallpaper_feather == 0x0D0040C0);
    CHECK(theme.shadow == 0x00000000);
}

TEST_CASE("every theme is stored at its own index", "[tokens]")
{
    for (uint32_t index = 0; index < UI::ThemeCount; ++index)
    {
        const UI::Theme& theme = UI::BaseTheme(static_cast<ThemeId>(index));
        CHECK(static_cast<uint32_t>(theme.id) == index);
        CHECK(UI::FamilyName(theme.family) != nullptr);
    }
}

TEST_CASE("WithAccent replaces accent, text on it and focus, and mixes accent-soft", "[tokens]")
{
    SECTION("dark theme, 30 percent")
    {
        const UI::Theme& base = UI::BaseTheme(ThemeId::Dusk);
        for (const AccentCase& expected : OnDusk)
        {
            UI::Theme theme = UI::WithAccent(base, expected.id);
            CHECK(theme.accent == expected.accent);
            CHECK(theme.on_accent == expected.on_accent);
            CHECK(theme.focus == expected.accent);
            CHECK(theme.accent_soft == expected.soft);
            CHECK(theme.surface == base.surface);
            CHECK(theme.text == base.text);
            CHECK(theme.id == base.id);
        }
    }
    SECTION("light theme, 18 percent")
    {
        const UI::Theme& base = UI::BaseTheme(ThemeId::Airy);
        for (const AccentCase& expected : OnAiry)
        {
            UI::Theme theme = UI::WithAccent(base, expected.id);
            CHECK(theme.accent == expected.accent);
            CHECK(theme.on_accent == expected.on_accent);
            CHECK(theme.focus == expected.accent);
            CHECK(theme.accent_soft == expected.soft);
        }
    }
    SECTION("Default leaves the theme as it is")
    {
        for (uint32_t index = 0; index < UI::ThemeCount; ++index)
        {
            const UI::Theme& base = UI::BaseTheme(static_cast<ThemeId>(index));
            UI::Theme theme = UI::WithAccent(base, AccentId::Default);
            CHECK(std::memcmp(&theme, &base, sizeof theme) == 0);
        }
    }
}

TEST_CASE("ResolveTheme switches to light from 7 to 18 inclusive", "[tokens]")
{
    CHECK(UI::ResolveTheme(Family::Dusk, Mode::Auto, 6) == ThemeId::Dusk);
    CHECK(UI::ResolveTheme(Family::Dusk, Mode::Auto, 7) == ThemeId::Airy);
    CHECK(UI::ResolveTheme(Family::Dusk, Mode::Auto, 18) == ThemeId::Airy);
    CHECK(UI::ResolveTheme(Family::Dusk, Mode::Auto, 19) == ThemeId::Dusk);
    CHECK(UI::ResolveTheme(Family::Paper, Mode::Auto, 12) == ThemeId::Paper);
    CHECK(UI::ResolveTheme(Family::Ember, Mode::Auto, 3) == ThemeId::Ember);
    CHECK(UI::ResolveTheme(Family::HighContrast, Mode::Auto, 12) == ThemeId::ContrastLight);
}

TEST_CASE("ResolveTheme with a fixed mode ignores the hour", "[tokens]")
{
    for (uint32_t hour : {0U, 6U, 7U, 12U, 18U, 19U, 23U})
    {
        CHECK(UI::ResolveTheme(Family::Ember, Mode::Dark, hour) == ThemeId::Ember);
        CHECK(UI::ResolveTheme(Family::Ember, Mode::Light, hour) == ThemeId::Terracotta);
        CHECK(UI::ResolveTheme(Family::HighContrast, Mode::Dark, hour) == ThemeId::ContrastDark);
    }
}

TEST_CASE("metrics, shadows, type roles and app tiles come from the tokens", "[tokens]")
{
    CHECK(UI::RadiusSmall == 4);
    CHECK(UI::RadiusMedium == 6);
    CHECK(UI::RadiusLarge == 10);
    CHECK(UI::BarHeight == 28);
    CHECK(UI::TitleBarHeight == 32);
    CHECK(UI::ControlHeight == 26);
    CHECK(UI::MenuRowHeight == 24);
    CHECK(UI::DurationFastMs == 80);
    CHECK(UI::DurationBaseMs == 120);
    CHECK(UI::DurationSlowMs == 180);
    CHECK(UI::EasingMilli[0] == 200);
    CHECK(UI::EasingMilli[3] == 1000);

    UI::ShadowSpec focused = UI::ShadowSpecOf(UI::ShadowRole::WindowFocused);
    CHECK(focused.offset_y == 10);
    CHECK(focused.blur == 26);
    CHECK(UI::ShadowSpecOf(UI::ShadowRole::Dialog).blur == 40);

    CHECK(std::strcmp(UI::FontName(UI::TypeRole::Body), "sans-16") == 0);
    CHECK(std::strcmp(UI::FontName(UI::TypeRole::Mono), "mono-16") == 0);
    CHECK(UI::AppTileColor(UI::AppId::Files) == 0xFF3D78B0);
    CHECK(UI::AppTileColor(UI::AppId::Settings) == 0xFF555D68);
}
