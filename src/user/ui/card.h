#ifndef FEATHEROS_USER_UI_CARD_H
#define FEATHEROS_USER_UI_CARD_H

#include "../graphics/canvas.h"
#include "tokens.h"

#include <stdint.h>

namespace FeatherOS::UI
{

constexpr uint32_t CardPageCount = 4;

// The folder the card's fonts (the type roles' files, "<name>.ffnt") are read from, with its
// trailing slash; the pointer must outlive every DrawCard. "/system/fonts/" without a call. A font
// that cannot be read is replaced by the console font.
void SetCardFontDirectory(const char* directory);

// Draws page 1 to 4 of the test card over the whole of `width` x `height` (the canvas's surface,
// with a fresh clip and origin). Page 1: the theme's colours, shapes and type roles; 2: the glyphs
// and the feather; 3: the app tiles and pointers; 4: the wallpaper with the shadows. The layout is
// fixed from the top-left except for page 4, which spreads its boxes over the size. Any other
// page draws nothing.
void DrawCard(Graphics::Canvas& canvas, int32_t width, int32_t height, uint32_t page, ThemeId theme,
              AccentId accent);

// The short ids of tools/gui/tokens.json ("dusk", "hcl", "blue"); false when there is no such id.
bool ParseThemeId(const char* text, ThemeId* theme);
bool ParseAccentId(const char* text, AccentId* accent);
const char* ThemeIdName(ThemeId theme);
const char* AccentIdName(AccentId accent);

} // namespace FeatherOS::UI

#endif
