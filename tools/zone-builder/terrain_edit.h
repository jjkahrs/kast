// Terrain math on a ZoneDoc's heights: picking the ground under a ray, sculpt brushes, heightRange re-quantizing and
// 16-bit PNG heightmaps. Pure, no window.
#pragma once
#include "zone_doc.h"

#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace zone {

using TileSet = std::set<TileKey, TileLess>;

// The heights of tile t (flat when absent).
const kast::map::Heights& tile_heights(const ZoneState& state, TileKey t);

// Ground height at map (x, z) (the triangle under it, as kast::map::ground); nullopt off the map.
std::optional<float> ground_y(const kast::map::MapInfo& info, const ZoneState& state, float x, float z);

// First hit of the ray origin + t * dir (t in [0, maxDist]) with the ground triangles of the map's declared tiles.
std::optional<Vec3> raycast(const kast::map::MapInfo& info, const ZoneState& state, Vec3 origin, Vec3 dir, float maxDist);

enum class BrushMode { Raise, Lower, Flatten };

struct Brush {
    BrushMode mode = BrushMode::Raise;
    float radius = 16;   // metres
    float falloff = 0.5f; // 0: full strength to the edge; 1: fades from the centre
    float target = 0;    // Flatten: the height (metres) to pull toward
};

// Brush weight at distance d from the centre: 1 inside radius * (1 - falloff), smoothstep down to 0 at radius.
float brush_weight(const Brush& brush, float d);

// One dab of brush at map (x, z). amount: Raise/Lower: metres at full weight; Flatten: the fraction (0..1) of the way
// to target at full weight. Every sample is written to every declared tile that holds it (shared edges stay equal), as
// new buffers (copy on write), clamped to the heightRange. Returns the tiles it changed.
TileSet apply_brush(const kast::map::MapInfo& info, ZoneState& state, float x, float z, const Brush& brush, float amount);

// Samples that fall outside [newMin, newMax] when the map's heights move to that range.
size_t count_clipped(const kast::map::MapInfo& info, const ZoneState& state, float newMin, float newMax);

// Sets the manifest's heightRange to [newMin, newMax] and re-quantizes every declared tile (flat ones too, if their
// height moves) so world heights stay put, within one step of the new range; samples outside it clip. Returns how many
// clipped.
size_t set_height_range(const kast::map::MapInfo& info, ZoneState& state, float newMin, float newMax);

// A 257 x 257 16-bit grayscale PNG of a tile's heights (stored deflate: big, exact and dependency-free).
std::string encode_png16(const kast::map::Heights& heights);

// Replaces tile's heights with a 257 x 257 16-bit grayscale PNG's samples, then copies its edges onto the declared
// neighbours' shared edges so the map has no seams. false with err set ("png: 8-bit ...") if the PNG is not one.
bool import_png16(const kast::map::MapInfo& info, ZoneState& state, TileKey tile, std::string_view png, std::string& err);

// --- tiles (map.json "tiles") ---

// The declared tiles, sorted.
TileSet declared_tiles(const kast::map::MapInfo& info);
// The fewest-ish rects covering tiles: runs along x per row, merged with the next rows that have the same run.
Json tile_rects(const TileSet& tiles);
// Declares tile t (false if it already is). Its heights start flat at heightMin, then take the shared edges of the
// declared neighbours, so no seam appears.
bool add_tile(const kast::map::MapInfo& info, ZoneState& state, TileKey t);
// Undeclares tile t (false if it is not declared, or is the last one) and drops its heights and spawn file.
bool remove_tile(const kast::map::MapInfo& info, ZoneState& state, TileKey t);

} // namespace zone
