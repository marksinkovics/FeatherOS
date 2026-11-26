#include "art.h"

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace FeatherOS::UI
{

namespace
{

using Graphics::OutlineCache;
using Graphics::Outlines;
using Graphics::OutlineStatus;

struct ArtState
{
    Outlines outlines;
    int32_t glyphs[GlyphCount] = {};
    int32_t feather = -1;
    int32_t wallpaper = -1;
    int32_t pointers[PointerDrawingCount] = {};
    std::atomic<bool> loaded{false};
};

ArtState& State()
{
    static ArtState state;
    return state;
}

OutlineStatus LoadInto(ArtState& state, const char* path)
{
    const OutlineStatus status = state.outlines.Load(path);
    for (uint32_t glyph = 0; glyph < GlyphCount; ++glyph)
    {
        state.glyphs[glyph] = state.outlines.Find(GlyphDrawingNames[glyph]);
    }
    state.feather = state.outlines.Find("feather");
    state.wallpaper = state.outlines.Find("wallpaper");
    for (uint32_t drawing = 0; drawing < PointerDrawingCount; ++drawing)
    {
        state.pointers[drawing] = state.outlines.Find(PointerDrawingNames[drawing]);
    }
    state.loaded.store(true, std::memory_order_release);
    return status;
}

const ArtState& Ready()
{
    static std::once_flag once;
    ArtState& state = State();
    std::call_once(once,
                   [&state]
                   {
                       if (!state.loaded.load(std::memory_order_acquire))
                       {
                           LoadInto(state, ArtFilePath);
                       }
                   });
    return state;
}

void DrawMissing(Graphics::Canvas& canvas, const Graphics::Rectangle& destination,
                 Graphics::Color color)
{
    canvas.StrokeRoundedRectangle(destination, 0, 1, color);
}

void DrawGlyphWith(Graphics::Canvas& canvas, GlyphId glyph, const Graphics::Rectangle& destination,
                   Graphics::Color color, const Graphics::OutlineOptions& options)
{
    if (destination.width <= 0 || destination.height <= 0)
    {
        return;
    }
    const ArtState& art = Ready();
    const uint32_t id = static_cast<uint32_t>(glyph);
    const int32_t index = id < GlyphCount ? art.glyphs[id] : -1;
    if (index < 0)
    {
        DrawMissing(canvas, destination, color);
        return;
    }
    if (const Graphics::Mask* mask =
            ArtCache().GetMask(art.outlines, index, destination.width, destination.height, options))
    {
        canvas.DrawMask(*mask, destination.x, destination.y, color);
        return;
    }
    // Too big for the cache: drawn straight from the outline.
    Graphics::OutlinePaints paints;
    paints.current = color;
    canvas.DrawOutline(art.outlines, index, destination, paints, options);
}

constexpr uint32_t FoldAlpha = 41;     // 16% of 255, rounded
constexpr uint32_t HairlineAlpha = 56; // 22% of 255, rounded
// 2.3 units in 1/256, rounded to nearest (588.8).
constexpr int32_t SmallStrokeWidth = 589;

constexpr int32_t TileRadius(int32_t size)
{
    return (size * 23 + 50) / 100;
}

constexpr int32_t TileGlyphSize(int32_t size)
{
    return (size * 56 + 50) / 100;
}

// The hairline is the band |x + y - size| <= 0.75 * sqrt(2) around the diagonal, which is 1.5 px
// wide measured across it. Per pixel diagonal d = x + y - size, the share of the pixel's 8x8
// samples (centres at (2i + 1) / 16) inside the band, as 0 to 255. Squaring both sides of
// |16 d + 2 i + 2 j + 2| / 16 <= sqrt(9 / 8) avoids the root: the test is (...)^2 <= 288.
constexpr int32_t HairlineFirstDiagonal = -3;
constexpr int32_t HairlineDiagonals = 5;

void HairlineCoverage(uint8_t (&out)[HairlineDiagonals])
{
    for (int32_t k = 0; k < HairlineDiagonals; ++k)
    {
        const int32_t d = HairlineFirstDiagonal + k;
        int32_t inside = 0;
        for (int32_t i = 0; i < 8; ++i)
        {
            for (int32_t j = 0; j < 8; ++j)
            {
                const int32_t t = 16 * d + 2 * i + 2 * j + 2;
                inside += t * t <= 288;
            }
        }
        out[k] = static_cast<uint8_t>((inside * 255 + 32) / 64);
    }
}

constexpr uint8_t Scale255(uint32_t a, uint32_t b)
{
    return static_cast<uint8_t>((a * b + 127) / 255);
}

// The fold and the hairline as coverage of the whole tile, each multiplied by the rounded
// square's own coverage (the same corner rows FillRoundedRectangle uses) so neither can reach
// outside it. The fold's edge is the diagonal x + y = size: a pixel on or past it is full, the
// one just before it is exactly half (the diagonal splits it into equal triangles).
bool BuildFoldMasks(int32_t size, int32_t radius, Graphics::Mask& fold, Graphics::Mask& hairline)
{
    if (!fold.Valid() || !hairline.Valid())
    {
        return false;
    }
    uint8_t band[HairlineDiagonals];
    HairlineCoverage(band);
    const int32_t r = std::min(radius, size / 2);
    Graphics::CornerCoverage corner(r);
    std::vector<uint8_t> square(static_cast<size_t>(size));
    for (int32_t y = 0; y < size; ++y)
    {
        std::fill(square.begin(), square.end(), uint8_t{255});
        const int32_t row = y < r ? y : (y >= size - r ? size - 1 - y : -1);
        if (row >= 0)
        {
            const uint8_t* shape = corner.Row(row);
            for (int32_t x = 0; x < r; ++x)
            {
                square[static_cast<size_t>(x)] = shape[x];
                square[static_cast<size_t>(size - 1 - x)] = shape[x];
            }
        }
        for (int32_t x = 0; x < size; ++x)
        {
            const uint32_t cover = square[static_cast<size_t>(x)];
            const int32_t d = x + y - size;
            if (d >= -1)
            {
                fold.SetCoverage(x, y, d >= 0 ? static_cast<uint8_t>(cover) : Scale255(cover, 128));
            }
            if (d >= HairlineFirstDiagonal && d < HairlineFirstDiagonal + HairlineDiagonals)
            {
                hairline.SetCoverage(x, y, Scale255(cover, band[d - HairlineFirstDiagonal]));
            }
        }
    }
    return true;
}

} // namespace

Graphics::OutlineStatus LoadArt(const char* path)
{
    return LoadInto(State(), path);
}

const Graphics::Outlines& Art()
{
    return Ready().outlines;
}

Graphics::OutlineCache& ArtCache()
{
    static thread_local OutlineCache cache;
    return cache;
}

void DrawGlyph(Graphics::Canvas& canvas, GlyphId glyph, const Graphics::Rectangle& destination,
               Graphics::Color color)
{
    DrawGlyphWith(canvas, glyph, destination, color, {});
}

void DrawFeather(Graphics::Canvas& canvas, const Graphics::Rectangle& destination,
                 Graphics::Color feather, Graphics::Color shaft)
{
    if (destination.width <= 0 || destination.height <= 0)
    {
        return;
    }
    const ArtState& art = Ready();
    if (art.feather < 0)
    {
        DrawMissing(canvas, destination, feather);
        return;
    }
    Graphics::OutlinePaints paints;
    paints.current = feather;
    paints.slots[0] = shaft;
    if (const Graphics::Surface* image = ArtCache().GetImage(
            art.outlines, art.feather, destination.width, destination.height, paints))
    {
        canvas.DrawSurface(*image, destination.x, destination.y);
        return;
    }
    canvas.DrawOutline(art.outlines, art.feather, destination, paints);
}

AppTile MakeAppTile(AppId app, const Theme& theme)
{
    static constexpr GlyphId Glyphs[AppCount] = {
        GlyphId::Folder, GlyphId::Term,    GlyphId::Note,  GlyphId::Calc,   GlyphId::Blocks,
        GlyphId::Lamp,   GlyphId::Sliders, GlyphId::Pulse, GlyphId::Shapes,
    };
    const uint32_t index = static_cast<uint32_t>(app);
    AppTile tile;
    tile.glyph = Glyphs[index < AppCount ? index : 0];
    tile.color = AppTileColor(app);
    tile.high_contrast = theme.family == Family::HighContrast;
    tile.surface = theme.surface;
    tile.text = theme.text;
    return tile;
}

void DrawAppTile(Graphics::Canvas& canvas, const Graphics::Rectangle& destination,
                 const AppTile& tile, TileStyle style)
{
    const int32_t size = std::min(destination.width, destination.height);
    if (size <= 0)
    {
        return;
    }
    const Graphics::Rectangle square = {destination.x + (destination.width - size) / 2,
                                        destination.y + (destination.height - size) / 2, size,
                                        size};
    const int32_t radius = TileRadius(size);
    const Graphics::Color glyph_color = tile.high_contrast ? tile.text : 0xFFFFFFFFU;
    if (tile.high_contrast)
    {
        canvas.FillRoundedRectangle(square, radius, tile.surface);
        canvas.StrokeRoundedRectangle(square, radius, ContrastBorderWidth, tile.text);
    }
    else
    {
        canvas.FillRoundedRectangle(square, radius, tile.color);
        if (style == TileStyle::Full)
        {
            Graphics::Mask fold(size, size);
            Graphics::Mask hairline(size, size);
            if (BuildFoldMasks(size, radius, fold, hairline))
            {
                canvas.DrawMask(fold, square.x, square.y, Graphics::WithAlpha(0, FoldAlpha));
                canvas.DrawMask(hairline, square.x, square.y,
                                Graphics::WithAlpha(0xFFFFFFFFU, HairlineAlpha));
            }
        }
    }
    const int32_t glyph_size = TileGlyphSize(size);
    const int32_t offset = (size - glyph_size) / 2;
    Graphics::OutlineOptions options;
    if (style == TileStyle::Small)
    {
        options.stroke_width = SmallStrokeWidth;
    }
    DrawGlyphWith(canvas, tile.glyph,
                  {square.x + offset, square.y + offset, glyph_size, glyph_size}, glyph_color,
                  options);
}

PointerPoint PointerHotspot(PointerId pointer, int32_t size)
{
    const uint32_t id = static_cast<uint32_t>(pointer);
    if (id >= PointerCount || size <= 0)
    {
        return {0, 0};
    }
    const ArtState& art = Ready();
    const int32_t index = art.pointers[PointerFirstDrawing[id]];
    if (index < 0)
    {
        return {0, 0};
    }
    const Graphics::OutlineHotspot hotspot = art.outlines.Hotspot(index);
    if (!hotspot.present)
    {
        return {0, 0};
    }
    // The file's hotspot is in 1/256 of a unit of the 24-unit view box.
    constexpr int64_t Unit = 256;
    const auto scale = [size](int32_t value)
    { return static_cast<int32_t>((int64_t{value} * size + 12 * Unit) / (24 * Unit)); };
    return {scale(hotspot.x), scale(hotspot.y)};
}

uint32_t PointerFrameCount(PointerId pointer)
{
    const uint32_t id = static_cast<uint32_t>(pointer);
    return id < PointerCount ? PointerFrames[id] : 1;
}

void DrawPointer(Graphics::Canvas& canvas, PointerId pointer, int32_t x, int32_t y, int32_t size,
                 Graphics::Color accent, uint32_t frame)
{
    const uint32_t id = static_cast<uint32_t>(pointer);
    if (size <= 0 || id >= PointerCount)
    {
        return;
    }
    const ArtState& art = Ready();
    const PointerPoint hotspot = PointerHotspot(pointer, size);
    const Graphics::Rectangle destination = {x - hotspot.x, y - hotspot.y, size, size};
    const int32_t index = art.pointers[PointerFirstDrawing[id] + frame % PointerFrames[id]];
    if (index < 0)
    {
        DrawMissing(canvas, destination, accent);
        return;
    }
    Graphics::OutlinePaints paints;
    paints.slots[0] = accent;
    if (const Graphics::Surface* image =
            ArtCache().GetImage(art.outlines, index, size, size, paints))
    {
        canvas.DrawSurface(*image, destination.x, destination.y);
        return;
    }
    canvas.DrawOutline(art.outlines, index, destination, paints);
}

namespace
{

// The wallpaper's design grid; the paint slots are those of tools/gui/art/wallpaper.json.
constexpr int64_t WallpaperWidth = 1600;
constexpr int64_t WallpaperHeight = 1000;

struct ShadowTable
{
    std::vector<Graphics::NineSlice> slices;

    ShadowTable()
    {
        slices.reserve(ShadowRoleCount);
        for (uint32_t role = 0; role < ShadowRoleCount; ++role)
        {
            const ShadowSpec& spec = ShadowSpecOf(static_cast<ShadowRole>(role));
            slices.push_back(Graphics::MakeShadow(spec.corner_radius, spec.blur));
        }
    }
};

} // namespace

void DrawWallpaper(Graphics::Surface* surface, const Theme& theme)
{
    if (surface == nullptr || !surface->Valid())
    {
        return;
    }
    Graphics::Canvas canvas(*surface);
    const int64_t width = surface->Width();
    const int64_t height = surface->Height();
    canvas.FillRectangle({0, 0, surface->Width(), surface->Height()}, theme.desk);
    const ArtState& art = Ready();
    if (art.wallpaper < 0)
    {
        return;
    }
    // The larger scale is width / 1600 when width * 1000 >= height * 1600; the other side rounds
    // up so that the drawing never falls short of the surface.
    int64_t drawn_width = width;
    int64_t drawn_height = (width * WallpaperHeight + WallpaperWidth - 1) / WallpaperWidth;
    if (width * WallpaperHeight < height * WallpaperWidth)
    {
        drawn_height = height;
        drawn_width = (height * WallpaperWidth + WallpaperHeight - 1) / WallpaperHeight;
    }
    const auto left = static_cast<int32_t>(-((drawn_width - width) / 2));
    const auto top = static_cast<int32_t>(-((drawn_height - height) / 2));
    Graphics::OutlinePaints paints;
    paints.slots[0] = theme.wallpaper_feather;
    paints.slots[1] = theme.desk;
    paints.slots[2] = theme.wallpaper_1;
    paints.slots[3] = theme.wallpaper_2;
    paints.slots[4] = theme.wallpaper_3;
    canvas.DrawOutline(
        art.outlines, art.wallpaper,
        {left, top, static_cast<int32_t>(drawn_width), static_cast<int32_t>(drawn_height)}, paints);
}

const Graphics::NineSlice& ShadowFor(ShadowRole role)
{
    static const ShadowTable table;
    const uint32_t index = static_cast<uint32_t>(role);
    return table.slices[index < ShadowRoleCount ? index : 0];
}

} // namespace FeatherOS::UI
