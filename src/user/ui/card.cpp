#include "card.h"

#include "../graphics/dirty_region.h"
#include "../graphics/font.h"
#include "art.h"

#include <stddef.h>
#include <string.h>

namespace FeatherOS::UI
{

namespace
{

using Graphics::Canvas;
using Graphics::Color;
using Graphics::Font;
using Graphics::Rectangle;

const char* g_font_directory = "/system/fonts/";

constexpr const char* ThemeIds[ThemeCount] = {"dusk",  "airy",  "ink", "paper",
                                              "ember", "terra", "hcd", "hcl"};
constexpr const char* AccentIds[AccentCount] = {"default", "blue",  "teal", "green",
                                                "amber",   "coral", "pink", "violet"};

constexpr int32_t Margin = 16;
constexpr int32_t HeaderHeight = 28;

struct Fonts
{
    Font loaded[TypeRoleCount];
    const Font* font[TypeRoleCount];

    Fonts()
    {
        char path[160];
        const size_t directory = strlen(g_font_directory);
        for (uint32_t role = 0; role < TypeRoleCount; ++role)
        {
            const char* name = FontName(static_cast<TypeRole>(role));
            font[role] = Font::Console();
            if (directory + strlen(name) + 6 > sizeof(path))
            {
                continue;
            }
            strcpy(path, g_font_directory);
            strcat(path, name);
            strcat(path, ".ffnt");
            if (loaded[role].Load(path) == FeatherOS::Text::FontStatus::Ok)
            {
                font[role] = &loaded[role];
            }
        }
    }

    const Font& Of(TypeRole role) const
    {
        return *font[static_cast<uint32_t>(role)];
    }
};

// A small string builder, so the card needs no printf.
class Line
{
public:
    Line& Add(const char* text)
    {
        while (*text != '\0' && m_length + 1 < sizeof(m_buffer))
        {
            m_buffer[m_length++] = *text++;
        }
        m_buffer[m_length] = '\0';
        return *this;
    }

    Line& Add(uint32_t number)
    {
        char digits[10];
        size_t count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + number % 10);
            number /= 10;
        } while (number != 0);
        while (count > 0 && m_length + 1 < sizeof(m_buffer))
        {
            m_buffer[m_length++] = digits[--count];
        }
        m_buffer[m_length] = '\0';
        return *this;
    }

    const char* Get() const
    {
        return m_buffer;
    }

private:
    char m_buffer[96] = {};
    size_t m_length = 0;
};

struct Context
{
    Canvas& canvas;
    int32_t width;
    int32_t height;
    uint32_t page;
    const Theme& theme;
    const Fonts& fonts;
};

void Label(const Context& context, int32_t x, int32_t y, const char* text, Color color)
{
    context.canvas.DrawText(context.fonts.Of(TypeRole::Mono), x, y, text, color);
}

void Header(const Context& context, const char* what)
{
    Line line;
    line.Add("Test card ")
        .Add(context.page)
        .Add(" of ")
        .Add(CardPageCount)
        .Add(": ")
        .Add(what)
        .Add(" - ")
        .Add(context.theme.name);
    context.canvas.DrawText(context.fonts.Of(TypeRole::Body), Margin, 6, line.Get(),
                            context.theme.text);
}

int32_t LineStep(const Font& font)
{
    return font.LineHeight() + 4;
}

void DrawColours(const Context& context)
{
    const Theme& theme = context.theme;
    struct Swatch
    {
        const char* name;
        Color color;
    };
    const Swatch swatches[16] = {
        {"desk", theme.desk},
        {"bar", theme.bar},
        {"surface", theme.surface},
        {"raised", theme.raised},
        {"sunken", theme.sunken},
        {"line", theme.line},
        {"text", theme.text},
        {"muted", theme.muted},
        {"accent", theme.accent},
        {"on_accent", theme.on_accent},
        {"accent_soft", theme.accent_soft},
        {"ok", theme.ok},
        {"warn", theme.warn},
        {"bad", theme.bad},
        {"info", theme.info},
        {"focus", theme.focus},
    };
    const Font& mono = context.fonts.Of(TypeRole::Mono);
    const int32_t pitch_x = 124;
    const int32_t pitch_y = 40 + LineStep(mono) + 4;
    for (int32_t index = 0; index < 16; ++index)
    {
        const int32_t x = Margin + (index % 8) * pitch_x;
        const int32_t y = HeaderHeight + (index / 8) * pitch_y;
        const Rectangle box = {x, y, 116, 40};
        context.canvas.FillRoundedRectangle(box, RadiusMedium, swatches[index].color);
        context.canvas.StrokeRoundedRectangle(box, RadiusMedium, 1, theme.muted);
        Label(context, x, y + 44, swatches[index].name, theme.text);
    }
}

int32_t DrawShapes(const Context& context, int32_t y)
{
    Canvas& canvas = context.canvas;
    const Theme& theme = context.theme;
    Label(context, Margin, y, "radius 0 4 6 10 24, filled then stroked 2", theme.muted);
    y += 24;
    const int32_t radii[5] = {0, RadiusSmall, RadiusMedium, RadiusLarge, 24};
    for (int32_t index = 0; index < 5; ++index)
    {
        const int32_t x = Margin + index * 80;
        canvas.FillRoundedRectangle({x, y, 72, 48}, radii[index], theme.accent);
        canvas.StrokeRoundedRectangle({x + 400, y, 72, 48}, radii[index], 2, theme.text);
    }
    y += 64;
    Label(context, Margin, y, "pills, circles, lines, a translucent fill", theme.muted);
    y += 24;
    canvas.FillRoundedRectangle({Margin, y, 120, 28}, 14, theme.accent);
    canvas.StrokeRoundedRectangle({Margin + 128, y, 120, 28}, 14, 2, theme.text);
    canvas.FillRoundedRectangle({Margin + 264, y - 4, 36, 36}, 18, theme.ok);
    canvas.StrokeRoundedRectangle({Margin + 308, y - 4, 36, 36}, 18, 3, theme.bad);
    canvas.DrawHorizontalLine(Margin + 368, y + 2, 160, theme.text);
    canvas.DrawHorizontalLine(Margin + 368, y + 10, 160, theme.muted);
    canvas.DrawHorizontalLine(Margin + 368, y + 18, 160, theme.line);
    const Color verticals[4] = {theme.info, theme.warn, theme.ok, theme.bad};
    for (int32_t index = 0; index < 4; ++index)
    {
        canvas.DrawVerticalLine(Margin + 544 + index * 6, y, 28, verticals[index]);
    }
    canvas.FillRoundedRectangle({Margin + 600, y, 80, 28}, RadiusSmall, theme.text);
    canvas.FillRoundedRectangle({Margin + 640, y - 4, 80, 28}, RadiusSmall,
                                Graphics::WithAlpha(theme.warn, 0x80));
    return y + 40;
}

int32_t DrawTypeRoles(const Context& context, int32_t y)
{
    static const struct
    {
        TypeRole role;
        const char* name;
    } roles[TypeRoleCount] = {
        {TypeRole::Body, "Body"},       {TypeRole::Heading, "Heading"}, {TypeRole::Title, "Title"},
        {TypeRole::Display, "Display"}, {TypeRole::Clock, "12:48"},     {TypeRole::Mono, "Mono"},
    };
    Label(context, Margin, y, "type roles", context.theme.muted);
    y += 24;
    for (const auto& entry : roles)
    {
        const Font& font = context.fonts.Of(entry.role);
        Line line;
        line.Add(entry.name).Add(" Hamburgefonstiv 0123");
        context.canvas.DrawText(font, Margin, y, line.Get(), context.theme.text);
        y += LineStep(font);
    }
    return y + 4;
}

void DrawEllipsisCases(const Context& context, int32_t y)
{
    Canvas& canvas = context.canvas;
    const Theme& theme = context.theme;
    const Font& font = context.fonts.Of(TypeRole::Body);
    Label(context, Margin, y, "ellipsis: fits, exact, cut, no room", theme.muted);
    y += 24;
    const char* const fits = "Short text";
    const char* const exact = "Fits exactly";
    const char* const cut = "A line of text far too long for its box";
    const int32_t exact_width = font.MeasureText(exact);
    const struct
    {
        const char* text;
        int32_t width;
    } cases[4] = {{fits, 200}, {exact, exact_width}, {cut, 200}, {cut, 6}};
    int32_t x = Margin;
    for (const auto& entry : cases)
    {
        const Rectangle box = {x, y, entry.width, font.LineHeight()};
        canvas.FillRectangle({x - 4, y - 4, entry.width + 8, font.LineHeight() + 8}, theme.sunken);
        canvas.DrawTextEllipsis(font, box, entry.text, theme.text);
        x += entry.width + 24;
    }
}

void DrawPageColours(const Context& context)
{
    Header(context, "colours, shapes, type");
    DrawColours(context);
    int32_t y = HeaderHeight + 2 * (40 + LineStep(context.fonts.Of(TypeRole::Mono)) + 4) + 4;
    y = DrawShapes(context, y);
    y = DrawTypeRoles(context, y);
    DrawEllipsisCases(context, y);
}

constexpr int32_t GlyphSizes[7] = {12, 14, 16, 20, 24, 32, 64};
constexpr int32_t FeatherSizes[3] = {17, 56, 104};

void DrawPageGlyphs(const Context& context)
{
    Header(context, "glyphs and the feather");
    Canvas& canvas = context.canvas;
    const Theme& theme = context.theme;
    const int32_t grid_x = 48;
    const int32_t usable = context.width - grid_x - Margin;
    int32_t y = HeaderHeight;
    for (const int32_t size : GlyphSizes)
    {
        const int32_t pitch = size + 4;
        int32_t columns = usable / pitch;
        columns = columns > 0 ? columns : 1;
        Label(context, Margin, y, Line().Add(static_cast<uint32_t>(size)).Get(), theme.muted);
        for (uint32_t glyph = 0; glyph < GlyphCount; ++glyph)
        {
            const int32_t column = static_cast<int32_t>(glyph) % columns;
            const int32_t row = static_cast<int32_t>(glyph) / columns;
            DrawGlyph(canvas, static_cast<GlyphId>(glyph),
                      {grid_x + column * pitch, y + row * pitch, size, size}, theme.text);
        }
        const int32_t rows = (static_cast<int32_t>(GlyphCount) + columns - 1) / columns;
        y += rows * pitch + 6;
    }
    Label(context, Margin, y, "feather 17 56 104", theme.muted);
    y += 24;
    int32_t x = Margin;
    for (const int32_t size : FeatherSizes)
    {
        DrawFeather(canvas, {x, y, size, size}, theme.accent, theme.text);
        x += size + 24;
    }
}

constexpr int32_t TileSizes[5] = {18, 24, 28, 44, 64};
constexpr TileStyle TileStyles[3] = {TileStyle::Full, TileStyle::Small, TileStyle::Plain};
constexpr const char* TileStyleNames[3] = {"Full", "Small", "Plain"};
constexpr int32_t PointerSizes[3] = {24, 32, 48};

void DrawPageTiles(const Context& context)
{
    Header(context, "app tiles and pointers");
    Canvas& canvas = context.canvas;
    const Theme& theme = context.theme;
    const int32_t group_pitch = 3 * 72 + 16;
    int32_t y = HeaderHeight;
    for (int32_t group = 0; group < 3; ++group)
    {
        Label(context, Margin + group * group_pitch, y, TileStyleNames[group], theme.muted);
    }
    y += 24;
    for (int32_t row = 0; row < 5; ++row)
    {
        const int32_t size = TileSizes[row];
        for (int32_t group = 0; group < 3; ++group)
        {
            for (int32_t index = 0; index < 3; ++index)
            {
                const auto app = static_cast<AppId>(((group + row) % 3) * 3 + index);
                DrawAppTile(canvas, {Margin + group * group_pitch + index * 72, y, size, size},
                            MakeAppTile(app, theme), TileStyles[group]);
            }
        }
        y += size + 8;
    }
    Label(context, Margin, y, "pointers 24 32 48, the spinners' 8 frames at 32", theme.muted);
    y += 24;
    for (const int32_t size : PointerSizes)
    {
        canvas.FillRectangle({Margin, y - 4, 13 * 64 + 8, size + 8}, theme.surface);
        for (uint32_t pointer = 0; pointer < PointerCount; ++pointer)
        {
            const auto id = static_cast<PointerId>(pointer);
            const PointerPoint hotspot = PointerHotspot(id, size);
            DrawPointer(canvas, id, Margin + 8 + static_cast<int32_t>(pointer) * 64 + hotspot.x,
                        y + hotspot.y, size, theme.accent, 0);
        }
        y += size + 12;
    }
    canvas.FillRectangle({Margin, y - 4, 16 * 40 + 24, 32 + 8}, theme.surface);
    for (uint32_t frame = 0; frame < 8; ++frame)
    {
        const PointerPoint working = PointerHotspot(PointerId::Working, 32);
        const PointerPoint busy = PointerHotspot(PointerId::Busy, 32);
        const int32_t x = Margin + 8 + static_cast<int32_t>(frame) * 40;
        DrawPointer(canvas, PointerId::Working, x + working.x, y + working.y, 32, theme.accent,
                    frame);
        DrawPointer(canvas, PointerId::Busy, x + 8 * 40 + 16 + busy.x, y + busy.y, 32, theme.accent,
                    frame);
    }
}

constexpr const char* ShadowNames[ShadowRoleCount] = {
    "window unfocused", "window focused", "menu",    "dock",
    "notification",     "dialog",         "overlay", "thumbnail",
};

void DrawPageWallpaper(const Context& context)
{
    Canvas& canvas = context.canvas;
    const Theme& theme = context.theme;
    DrawWallpaper(&canvas.Target(), theme);
    const int32_t box_width = 160;
    const int32_t box_height = 96;
    const int32_t gap_x = (context.width - 4 * box_width) / 5;
    const int32_t gap_y = (context.height - 2 * box_height) / 3;
    for (int32_t index = 0; index < static_cast<int32_t>(ShadowRoleCount); ++index)
    {
        const auto role = static_cast<ShadowRole>(index);
        const ShadowSpec& spec = ShadowSpecOf(role);
        const Rectangle box = {gap_x + (index % 4) * (box_width + gap_x),
                               gap_y + (index / 4) * (box_height + gap_y), box_width, box_height};
        canvas.DrawNineSlice(ShadowFor(role), Graphics::ShadowBounds(box, spec.offset_y, spec.blur),
                             theme.shadow, false);
        canvas.FillRoundedRectangle(box, spec.corner_radius, theme.surface);
        canvas.StrokeRoundedRectangle(box, spec.corner_radius, 1, theme.line);
        Label(context, box.x + 8, box.y + 8, ShadowNames[index], theme.text);
        Label(context, box.x + 8, box.y + 28,
              Line()
                  .Add("dy ")
                  .Add(static_cast<uint32_t>(spec.offset_y))
                  .Add(" blur ")
                  .Add(static_cast<uint32_t>(spec.blur))
                  .Get(),
              theme.muted);
    }
}

} // namespace

void SetCardFontDirectory(const char* directory)
{
    g_font_directory = directory;
}

void DrawCard(Canvas& canvas, int32_t width, int32_t height, uint32_t page, ThemeId theme,
              AccentId accent)
{
    if (page < 1 || page > CardPageCount || width <= 0 || height <= 0)
    {
        return;
    }
    const Theme resolved = WithAccent(BaseTheme(theme), accent);
    const Fonts fonts;
    const Context context = {canvas, width, height, page, resolved, fonts};
    canvas.FillRectangle({0, 0, width, height}, resolved.desk);
    switch (page)
    {
    case 1:
        DrawPageColours(context);
        break;
    case 2:
        DrawPageGlyphs(context);
        break;
    case 3:
        DrawPageTiles(context);
        break;
    default:
        DrawPageWallpaper(context);
        break;
    }
}

bool ParseThemeId(const char* text, ThemeId* theme)
{
    for (uint32_t index = 0; text != nullptr && index < ThemeCount; ++index)
    {
        if (strcmp(text, ThemeIds[index]) == 0)
        {
            *theme = static_cast<ThemeId>(index);
            return true;
        }
    }
    return false;
}

bool ParseAccentId(const char* text, AccentId* accent)
{
    for (uint32_t index = 0; text != nullptr && index < AccentCount; ++index)
    {
        if (strcmp(text, AccentIds[index]) == 0)
        {
            *accent = static_cast<AccentId>(index);
            return true;
        }
    }
    return false;
}

const char* ThemeIdName(ThemeId theme)
{
    const uint32_t index = static_cast<uint32_t>(theme);
    return ThemeIds[index < ThemeCount ? index : 0];
}

const char* AccentIdName(AccentId accent)
{
    const uint32_t index = static_cast<uint32_t>(accent);
    return AccentIds[index < AccentCount ? index : 0];
}

} // namespace FeatherOS::UI
