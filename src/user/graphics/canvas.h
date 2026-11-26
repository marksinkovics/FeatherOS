#ifndef FEATHEROS_USER_GRAPHICS_CANVAS_H
#define FEATHEROS_USER_GRAPHICS_CANVAS_H

#include "color.h"
#include "corner_mask.h"
#include "dirty_region.h"
#include "mask.h"
#include "nine_slice.h"
#include "outline_renderer.h"
#include "outlines.h"
#include "surface.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Graphics
{

class Font;

// Draws into one Surface through an origin and a clip. Every coordinate a caller passes is
// relative to the origin; nothing is written outside the clip, which never leaves the surface.
class Canvas
{
public:
    static constexpr size_t MaximumDepth = 16;

    explicit Canvas(Surface& surface);

    // Save fails at MaximumDepth and changes nothing; its matching Restore is then a no-op.
    bool Save();
    void Restore();
    void ClipTo(const Rectangle& rectangle);
    void Translate(int32_t dx, int32_t dy);

    // The surface this canvas draws into, for drawings that take a whole Surface.
    Surface& Target() const;
    Rectangle Clip() const;
    int64_t OriginX() const;
    int64_t OriginY() const;

    void FillRectangle(const Rectangle& rectangle, Color color);
    void DrawSurface(const Surface& source, int32_t x, int32_t y);
    void DrawSurfacePart(const Surface& source, int32_t sx, int32_t sy, int32_t width,
                         int32_t height, int32_t x, int32_t y);
    void DrawSurfaceFaded(const Surface& source, int32_t x, int32_t y, uint8_t opacity);
    void DrawMask(const Mask& mask, int32_t x, int32_t y, Color color);

    // An area average of the source pixels each destination pixel covers when shrinking an axis,
    // the nearest source pixel when enlarging it.
    void DrawSurfaceScaled(const Surface& source, const Rectangle& destination);

    // The mask's corners are copied, its edges and centre stretched to fill `outer`. An `outer`
    // smaller than the two insets together shares its size between them in proportion. With
    // skip_centre the centre is left untouched.
    void DrawNineSlice(const NineSlice& slice, const Rectangle& outer, Color color,
                       bool skip_centre);

    // Draws the text with its top-left at (x, y) and returns the pen's x after it, relative to the
    // origin and saturated. The colour's alpha scales the glyphs' coverage. On an Xrgb surface
    // with an opaque colour the pixels equal Surface::DrawText's.
    int32_t DrawText(const Font& font, int32_t x, int32_t y, const char* utf8, Color color);
    // Draws the text from the rectangle's top-left, cut to the rectangle. A text wider than the
    // rectangle ends with U+2026 (three full stops in a font without it) after as many whole
    // code points as leave room for it; if even that does not fit, nothing is drawn.
    void DrawTextEllipsis(const Font& font, const Rectangle& rectangle, const char* utf8,
                          Color color);

    // A radius below 0 is 0 and one above half the smaller side is cut to it. A stroke of at
    // least half the smaller side is a fill.
    void FillRoundedRectangle(const Rectangle& rectangle, int32_t radius, Color color);
    void StrokeRoundedRectangle(const Rectangle& rectangle, int32_t radius, int32_t width,
                                Color color);
    void DrawHorizontalLine(int32_t x, int32_t y, int32_t length, Color color);
    void DrawVerticalLine(int32_t x, int32_t y, int32_t length, Color color);

    // Maps the drawing's view box onto the destination and draws its items in order. Nothing is
    // drawn for an index out of range, a side over MaximumOutlineSize or a destination outside
    // the clip; only the visible part is rasterized.
    void DrawOutline(const Outlines& outlines, int32_t index, const Rectangle& destination,
                     const OutlinePaints& paints, const OutlineOptions& options = {});

private:
    struct State
    {
        int64_t origin_x;
        int64_t origin_y;
        int32_t left;
        int32_t top;
        int32_t right;
        int32_t bottom;
    };

    struct Bounds
    {
        int64_t left;
        int64_t top;
        int64_t right;
        int64_t bottom;

        bool Empty() const
        {
            return left >= right || top >= bottom;
        }
    };

    Bounds Visible(int64_t x, int64_t y, int64_t width, int64_t height) const;
    void FillArea(int64_t x, int64_t y, int64_t width, int64_t height, Color color);
    // The square of one rounded corner whose top-left is at absolute (x, y). `inset` and `hole`
    // cut the inner shape of a stroke out of the coverage, in the unmirrored corner's own space.
    void PaintCorner(CornerCoverage& outer, CornerCoverage* hole, int32_t inset, int64_t x,
                     int64_t y, bool mirror_x, bool mirror_y, Color color);
    int64_t DrawRun(const Font& font, int64_t pen, int64_t y, const char* utf8, size_t length,
                    Color color);
    void Composite(const Surface& source, int32_t sx, int32_t sy, int32_t width, int32_t height,
                   int32_t x, int32_t y, uint32_t opacity);

    Surface& m_surface;
    State m_state;
    State m_saved[MaximumDepth];
    size_t m_depth;
    size_t m_refused;
};

} // namespace FeatherOS::Graphics

#endif
