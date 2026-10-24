#pragma once

#include "math3d.h"

#include <map.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// Mesh LOD selection math for the Renderer, GPU-free so self_check can test it. See docs/engine/instancing-and-culling.html.
namespace kast {

// A level is kept until the screen height leaves its range by this fraction of the boundary it crossed, so a model
// sitting at a threshold doesn't flicker between two levels.
constexpr float kLodHysteresis = 0.1f;

// The fraction of the view's height the box covers through viewMtx / projMtx (bx convention): its largest extent over
// the height the view sees at the distance of its center (perspective) or the view's height (orthographic). Over 1
// when it's larger than the screen; distance-based, so turning the camera doesn't change it.
float lodScreenHeight(const Aabb& bounds, const float* viewMtx, const float* projMtx);
// The same from the eye's world position (the view's inverse), for callers that pick many draws per view.
float lodScreenHeight(const Aabb& bounds, const bx::Vec3& eye, const float* projMtx);

// The level for a screen height: the first whose threshold (decreasing per level) it reaches; below the last, the last
// level, or thresholds.size() (not drawn) if cullBelow. previous: the level chosen last frame (-1: none), kept while
// screenHeight is within kLodHysteresis of its range. No thresholds: level 0.
int selectLod(float screenHeight, std::span<const float> thresholds, bool cullBelow, int previous);

// Terrain LOD (Renderer::addTerrainDraws): a tile's kTileSamples - 1 quads per edge are split into chunks of
// kTerrainChunkQuads^2 quads over the tile's one vertex buffer, each drawn at an index stride of 1, 2, 4 or 8 samples
// from one index buffer per stride shared by every tile.
constexpr int kTerrainChunkQuads = 32;
constexpr int kTerrainChunksPerEdge = (map::kTileSamples - 1) / kTerrainChunkQuads;
constexpr int kTerrainChunks = kTerrainChunksPerEdge * kTerrainChunksPerEdge;
constexpr int kTerrainStrides = 4; // levels: stride 1 << level
// A stride is taken once its height error covers at most this fraction of the view's height (about 1.5 px at 720p)...
constexpr float kTerrainMaxScreenError = 0.002f;
// ...and the chunk is at least this many metres away per sample of stride (2: 64 m, 4: 128 m, 8: 256 m), so near
// terrain keeps its vertex normals (shading) even where it is flat.
constexpr float kTerrainStrideDistance = 32.0f;
// Skirt vertices, after the kTileSamples^2 grid ones (terrainSkirtVertex).
constexpr uint32_t kTerrainSkirtVertices = 2 * (kTerrainChunksPerEdge + 1) * map::kTileSamples;

static_assert(kTerrainChunksPerEdge * kTerrainChunkQuads == map::kTileSamples - 1, "chunks must tile the tile");
static_assert(kTerrainChunkQuads % (1 << (kTerrainStrides - 1)) == 0, "the coarsest stride must fit a chunk");

// Chunk's cell (u, v) in the tile, in chunks: chunks are numbered in Morton (Z) order, so every aligned 2x2, 4x4 block
// of chunks is a run of numbers (and of indices in the index buffers: draws merge runs of chunks at one stride).
std::pair<int, int> terrainChunkCell(int chunk);

// The skirt vertex under chunk-edge sample (u, v) (u or v a multiple of kTerrainChunkQuads): a copy of the grid vertex
// lowered by terrainSkirtDepth. Lines u = k * kTerrainChunkQuads first (kTileSamples each, by v), then v = k * ...
// (by u); a corner takes its u line's.
uint32_t terrainSkirtVertex(int u, int v);

// How far the skirt vertex under chunk-edge sample (u, v) hangs: twice the most any stride's edge is off the full one
// along the chunk edges through the sample (the gap between two neighbours' edges is at most the sum of theirs), plus
// kTerrainSkirtMargin for T-junction pinholes. Only those edges' samples count, which a neighbour tile shares, so both
// tiles' skirts reach. heights: as terrainChunkError.
// ponytail: an edge with nothing beyond it (the map's border, a tile not streamed in) shows its skirt from outside;
// the skirt faces out, so only a camera outside the loaded tiles sees it.
constexpr float kTerrainSkirtMargin = 0.25f;
float terrainSkirtDepth(std::span<const float> heights, int u, int v);

// Indices per chunk at a stride (1, 2, 4, 8), with or without skirts: every chunk has the same count, so chunk c starts
// at c * this.
uint32_t terrainChunkIndexCount(int stride, bool skirts);

// Appends chunk's triangles at stride over a tile's vertices (grid, then skirt): two per quad of stride^2 samples with
// map::for_each_triangle's winding and diagonal, then (skirts) a skirt quad hanging from each edge segment, facing out
// of the chunk, so a crack to a neighbour drawn at another stride (or a neighbour tile) shows skirt, not sky. Stride 1
// needs them too: where its edge is above a coarser neighbour's, the gap is seen from the neighbour's side, under its
// edge. Every chunk at one stride needs none: their edges match.
void appendTerrainChunk(int chunk, int stride, bool skirts, std::vector<uint32_t>& out);

// The most chunk's surface at stride is off its full-resolution one, in metres; heights: the tile's kTileSamples^2
// sample heights, row-major (v * kTileSamples + u).
float terrainChunkError(std::span<const float> heights, int chunk, int stride);

// The level (stride 1 << level) of a chunk with world bounds chunk seen from eye through projMtx (bx convention): the
// coarsest whose height error (errors[level], metres) covers at most kTerrainMaxScreenError of the view's height at the
// box's nearest point and which is at least kTerrainStrideDistance * stride metres from it. bias scales both as the
// LOD bias does (over 1 keeps detail).
// ponytail: no hysteresis (distance moves smoothly, so a pop happens once per crossing); add selectLod's band if a
// chunk is seen flickering at a boundary.
int terrainLevel(const Aabb& chunk, const bx::Vec3& eye, const float* projMtx,
                 std::span<const float, kTerrainStrides> errors, float bias);

} // namespace kast
