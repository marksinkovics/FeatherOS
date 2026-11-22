#ifndef FEATHEROS_USER_WICKDEEP_ART_H
#define FEATHEROS_USER_WICKDEEP_ART_H

#include "../graphics/tile_sheet.h"

#include <stdint.h>

namespace FeatherOS::User::WickdeepArt
{

constexpr int32_t TileSize = 16;
constexpr int32_t TileCount = 28;
constexpr int32_t PaletteSize = 16;

constexpr int32_t TileFloor = 0;
constexpr int32_t TileFloorCracked = 1;
constexpr int32_t TileWall = 2;
constexpr int32_t TileWallMossy = 3;
constexpr int32_t TileRubble = 4;
constexpr int32_t TileStairs = 5;
constexpr int32_t TilePlinth = 6;
constexpr int32_t TileKeeper = 7;
// One tile per kind, in the order of the Swift enums (first kind is 1).
constexpr int32_t TileFirstMonster = 8;
constexpr int32_t TileFirstItem = 16;
constexpr int32_t TileHeart = 26;
constexpr int32_t TileLamp = 27;

const uint32_t* Palette();
const char* const* TileRows();
Graphics::TileStatus LoadWickdeepTiles(Graphics::TileSheet* sheet);

} // namespace FeatherOS::User::WickdeepArt

#endif
