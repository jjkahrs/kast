// Terrain math on a ZoneDoc's heights: picking the ground under a ray, sculpt brushes, heightRange re-quantizing and
// 16-bit PNG heightmaps. Pure, no window.
#pragma once
#include "zone_doc.h"

#include <material.h>

#include <array>
#include <cstdint>
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

// --- random heights ---

struct NoiseParams {
    uint32_t seed = 1;
    float scale = 256;       // metres per noise feature (the first octave's wavelength)
    int octaves = 5;         // 1..16
    float persistence = 0.5f; // amplitude ratio between octaves, (0, 1]
    float lacunarity = 2.0f;  // frequency ratio between octaves, >= 1
};

// Replaces the heights of every declared tile with fBm Perlin noise sampled at each sample's map position (shared
// edges are the same position, so no seams), stretched over the whole heightRange. Same params and tiles: the same
// heights.
void generate_heights(const kast::map::MapInfo& info, ZoneState& state, const NoiseParams& params);

// --- height colors (map.json "terrainColors": [[pos, [r, g, b]], ...]; pos 0..1 of the heightRange, rgb 0..1) ---

struct ColorStop {
    float pos = 0;
    std::array<float, 3> rgb{};
};
using Gradient = std::vector<ColorStop>;

// Sand, grass, rock, snow.
Gradient default_gradient();
// The manifest's terrainColors; nullopt with err set if it has none or they are invalid (fewer than 2 stops, pos or a
// channel outside 0..1).
std::optional<Gradient> parse_gradient(const Json& manifest, std::string& err);
Json gradient_json(const Gradient& gradient);
// The color at t (0..1 of the heightRange): linear between the stops around it (in pos order), the end stops' beyond.
std::array<float, 3> gradient_color(const Gradient& gradient, float t);

// "materials/<folder>_terrain.mat", "textures/<folder>_terrain.png": the files Create texture & material assigns and
// save writes, project-relative.
std::string terrain_material_path(const std::string& folder);
std::string terrain_texture_path(const std::string& folder);

// RGB8, row 0 at the bounding box's min z.
struct TerrainImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};

// The gradient over the declared tiles' bounding box, 1 px per metre, shrunk to fit kMaxTerrainImage on the longest
// side; each pixel the color of the ground at its centre, grey off the map.
constexpr int kMaxTerrainImage = 4096;
TerrainImage bake_terrain(const kast::map::MapInfo& info, const ZoneState& state, const Gradient& gradient);
// An 8-bit RGB PNG (deflated).
std::string encode_png_rgb(const TerrainImage& image);
// The generated .mat: white base color times texturePath, metallic 0, roughness 0.9.
kast::Material terrain_material(const std::string& texturePath);

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
