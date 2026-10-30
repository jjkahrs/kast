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
// The Map tab's thumbnail of tile t: size x size pixels, row 0 at the tile's max z (north up, as the tab draws it),
// each the gradient's color at its nearest sample's height (grayscale, black low to white high, without one).
TerrainImage tile_thumbnail(const ZoneState& state, TileKey t, const std::optional<Gradient>& gradient, int size);
// The generated .mat: white base color times texturePath, metallic 0, roughness 0.9.
kast::Material terrain_material(const std::string& texturePath);

// --- tiles (map.json "tiles") ---

// The declared tiles, sorted.
TileSet declared_tiles(const kast::map::MapInfo& info);
// The editor's Scene view meshes the declared tiles within this many tiles (a square, kast::tiles_around) of the
// camera's pivot: 9 x 9 tiles, an 8 x 8 map whole when framed. The rest keep only their heights.
constexpr int kEditRadius = 4;
// Whether a tile's terrain mesh built under map a is still right under b: what the renderer reads from the map (id,
// height range, material, UV mode and, for Map UVs, the declared tiles' bounding box) is the same. A spawn, an area or a
// tile added elsewhere (Tiled UVs) keeps every mesh.
bool same_terrain_mesh(const kast::map::MapInfo& a, const kast::map::MapInfo& b);
// Whether ground point (x, z) is on the square of tiles the Scene view meshes around centre (kEditRadius tiles of the
// tile holding it, its outer edges included), declared or not: the zone overlays draw only there.
bool in_edit_view(std::array<float, 2> centre, float x, float z);
// The declared tiles' bounding box (inclusive; x0 > x1 with none).
kast::map::TileRect tile_bounds(const kast::map::MapInfo& info);

// --- tile grids (the Map tab's and the Zone tab's, zoomed and panned) ---

// A tile grid's view: cells `cell` pixels wide with `gap` pixels between them, (left, top) the point (in tiles: x, z)
// at the grid area's top-left corner. +z up, as seen from above.
struct GridView {
    float cell = 0, gap = 0;
    float left = 0, top = 0;
    float pitch() const { return cell + gap; }
};
// The view that shows bounds whole on an area cols x rows cells (at least bounds' size): cells as wide as fit `width`
// pixels on the longer side (minCell..maxCell; whole pixels from 8), bounds' bottom-left cell at the area's bottom-left corner.
GridView fit_grid(const kast::map::TileRect& bounds, int cols, int rows, float width, float gap, float minCell, float maxCell);
// The area cols x rows cells take in view's fitted cells: {width, height} pixels.
std::array<float, 2> grid_size(const GridView& view, int cols, int rows);
// view zoomed by factor (cells minCell..maxCell) about the area point `at` (pixels from its top-left): the ground under
// it stays there.
GridView zoom_grid(GridView view, float factor, std::array<float, 2> at, float minCell, float maxCell);
// view with the ground dragged by `pixels`, kept on bounds: an area of `size` pixels shows no cell past bounds' edges
// (its bottom-left corner when it is larger).
GridView pan_grid(GridView view, std::array<float, 2> pixels, const kast::map::TileRect& bounds, std::array<float, 2> size);
// view of `size` pixels centred on tile t at zoom times fit's cells (minCell..maxCell), for --grid-view.
GridView centre_grid(const GridView& fit, TileKey t, float zoom, std::array<float, 2> size, float minCell, float maxCell);
// The cells of bounds an area of `size` pixels shows (any part of them; x0 > x1 or z0 > z1 when none).
kast::map::TileRect visible_cells(const GridView& view, const kast::map::TileRect& bounds, std::array<float, 2> size);
// Tile t's cell's top-left corner, in pixels from the area's.
std::array<float, 2> cell_corner(const GridView& view, TileKey t);
// The cell under area point p (pixels from its top-left); nullopt in the gaps between cells.
std::optional<TileKey> cell_at(const GridView& view, std::array<float, 2> p);
// The fewest-ish rects covering tiles: runs along x per row, merged with the next rows that have the same run.
Json tile_rects(const TileSet& tiles);
// Declares tile t (false if it already is). Its heights start flat at heightMin, then take the shared edges of the
// declared neighbours, so no seam appears.
bool add_tile(const kast::map::MapInfo& info, ZoneState& state, TileKey t);
// Undeclares tile t (false if it is not declared, or is the last one) and drops its heights, spawn, objects and
// interactables files.
bool remove_tile(const kast::map::MapInfo& info, ZoneState& state, TileKey t);

} // namespace zone
