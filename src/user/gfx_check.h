#ifndef FEATHEROS_USER_GFX_CHECK_H
#define FEATHEROS_USER_GFX_CHECK_H

#include <stdint.h>

// What gfxcheck, a Swift program, draws with the graphics library: the checksum of the picture
// its steps make on a 64x40 surface, computed here without the library.
namespace FeatherOS::User::GfxCheck
{

constexpr int32_t Width = 64;
constexpr int32_t Height = 40;

constexpr int32_t TileSize = 8;
constexpr int32_t TileCount = 2;
constexpr int32_t PaletteSize = 4;
constexpr int32_t TilePictureWidth = 40;
constexpr int32_t TilePictureHeight = 20;

uint32_t ExpectedChecksum();

// The two-tile sheet gfxcheck loads, and the checksum of the 40x20 picture it draws from it.
const char* const* TileRows();
const uint32_t* TilePalette();
uint32_t ExpectedTileChecksum();

constexpr int32_t FontPictureWidth = 120;
constexpr int32_t FontPictureHeight = 48;

// The text gfxcheck draws in sans-16 and in the console font: accents, Greek, Cyrillic, box
// drawing, a code point the font lacks, a control and invalid UTF-8.
const char* FontText();
const char* OnText();
const char* ConsoleText();

// The checksum of the picture gfxcheck draws with the font read from `path` and with the console
// font, drawn here by plain loops over the file's bytes (0 if the file cannot be read).
uint32_t ExpectedFontChecksum(const char* path);

} // namespace FeatherOS::User::GfxCheck

#endif
