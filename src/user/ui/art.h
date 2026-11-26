#ifndef FEATHEROS_USER_UI_ART_H
#define FEATHEROS_USER_UI_ART_H

#include "../graphics/canvas.h"
#include "../graphics/color.h"
#include "../graphics/dirty_region.h"
#include "../graphics/outline_cache.h"
#include "../graphics/outlines.h"
#include "glyph_ids.h"
#include "pointer_ids.h"
#include "tokens.h"

namespace FeatherOS::UI
{

constexpr const char* ArtFilePath = "/system/gui/outlines.fvo";

// Replaces the process-wide outlines with the file at `path`. Without a call, the first drawing
// loads ArtFilePath. Not thread-safe against drawing: call it before any thread draws.
Graphics::OutlineStatus LoadArt(const char* path);

// The outlines every drawing below uses; invalid when the file is missing or damaged.
const Graphics::Outlines& Art();

// The cache of the calling thread, which the drawings below render through.
Graphics::OutlineCache& ArtCache();

// Draws the glyph's 24-unit view box scaled onto `destination` in `color`, through the cache. A
// missing file or a GlyphId out of range draws an empty box with a 1 px border.
void DrawGlyph(Graphics::Canvas& canvas, GlyphId glyph, const Graphics::Rectangle& destination,
               Graphics::Color color);

// The feather on its 64-unit view box: the vane and the quill in `feather`, the shaft in `shaft`
// (the bar's colour in the prototype). The same fallback as DrawGlyph.
void DrawFeather(Graphics::Canvas& canvas, const Graphics::Rectangle& destination,
                 Graphics::Color feather, Graphics::Color shaft);

enum class TileStyle : uint8_t
{
    // The fold and the hairline; for the dock and other large tiles.
    Full,
    // No fold, the glyph's stroke widened to 2.3 units: Quill rows, the Feather menu, switcher
    // labels.
    Small,
    // No fold, the normal stroke: Quill's detail pane.
    Plain,
};

// What a tile shows. In a high-contrast theme (`high_contrast`) the tile is `surface` with a
// border and glyph in `text` instead of `color` with a white glyph.
struct AppTile
{
    GlyphId glyph = GlyphId::Folder;
    Graphics::Color color = 0xFF555555U;
    bool high_contrast = false;
    Graphics::Color surface = 0xFF000000U;
    Graphics::Color text = 0xFFFFFFFFU;
};

// The app's glyph and colour, in the high-contrast form when `theme` is a high-contrast one.
AppTile MakeAppTile(AppId app, const Theme& theme);

// A rounded square of radius (size * 23 + 50) / 100 in the largest square that fits the
// destination, centred in it, with the glyph at (size * 56 + 50) / 100 centred in the square (the
// extra pixel of an odd difference goes right and down). The tile is composed live; a missing art
// file leaves the glyph as an empty box.
void DrawAppTile(Graphics::Canvas& canvas, const Graphics::Rectangle& destination,
                 const AppTile& tile, TileStyle style);

// A pointer's hotspot in pixels from its top-left at `size`, the file's hotspot at 24 px scaled as
// (hotspot * size + 12) / 24. (0, 0) for a PointerId out of range or a file without the drawing.
struct PointerPoint
{
    int32_t x;
    int32_t y;
};
PointerPoint PointerHotspot(PointerId pointer, int32_t size);

// The number of animation frames: 8 for Working and Busy, else 1.
uint32_t PointerFrameCount(PointerId pointer);

// Draws the pointer `size` px square with its hotspot at (x, y). The parts that follow the accent
// are painted in `accent`; `frame` picks the spinner's frame modulo its count and is ignored by
// the other pointers. A missing file draws an empty box.
void DrawPointer(Graphics::Canvas& canvas, PointerId pointer, int32_t x, int32_t y, int32_t size,
                 Graphics::Color accent, uint32_t frame);

// Fills `surface` with the theme's desk colour, then draws the wallpaper (the feather, its shaft
// and three ridges) scaled by the larger of the two scale factors so that it covers the surface,
// centred on it and cut at its edges. Drawn straight from the outline, never through the cache,
// which refuses a drawing this large. Without the art file only the desk colour is drawn; a null
// or invalid surface draws nothing.
void DrawWallpaper(Graphics::Surface* surface, const Theme& theme);

// The shadow of `role`, made once from ShadowSpecOf: the same object on every call. A role out of
// range gives the first one's.
const Graphics::NineSlice& ShadowFor(ShadowRole role);

} // namespace FeatherOS::UI

#endif
