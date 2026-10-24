#pragma once

#include "math3d.h"
#include "renderer.h"
#include "scene.h"
#include "voxel_gi.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <tuple>
#include <vector>

// SDFGI (docs/engine/sdfgi.html): diffuse GI and rough reflections for open worlds, with no bake.
//  - Cascades: SceneSdfgi::cascades nested grids of kSdfgiCells^3 cells (fewer in y with yScale) centered on the camera,
//    each cell twice the size of the one before (the first's: SceneSdfgi::cellSize). A cascade's origin snaps to its
//    probe spacing (kSdfgiProbeCells cells), so it scrolls in whole probe slabs; its textures are toroidal (a global
//    cell lives at texel cell mod dims), so a scroll rebuilds only the exposed slabs (sdfgiScrollRegions).
//  - SDF (cs_sdfgi_build, on the GPU): per cell, the signed distance (clamped at kSdfgiMaxDistance cells) to the nearest
//    Static shape (MeshRenderer primitives as oriented boxes / ellipsoids, static ModelRenderer submeshes as their world
//    bounds) or Terrain tile (heightfield: sdfgiHeightfieldDistance on the column's height and slope, sdfgiTerrainColumns),
//    with that surface's albedo and emission. The regions a Static entity or a tile leaves or enters are rebuilt the
//    frame it happens (SdfgiRenderer diffs this frame's shapes and tiles with the last).
//  - Light (cs_sdfgi_light, every frame): each cell on a surface is lit by the sun and up to kMaxVoxelGiLights local
//    lights (packVoxelGiLights; the falloffs fs_mesh.sc and VoxelGI use), each shadowed by marching the SDF toward it,
//    plus bounceFeedback times the probes' light there (so light bounces again each frame), plus its emission.
//  - Probes (cs_sdfgi_probe, every frame): a probe every kSdfgiProbeCells cells marches the preset's rays through the
//    cascades (leaving one, it goes on in the next); a ray that hits takes the lit cell's light, one that leaves them
//    all the sky (its harmonics, else the flat ambient). The rays' L1 harmonics blend into the probe's (temporal
//    accumulation, the preset's hysteresis). A probe inside geometry is marked invalid.
//  - Shading (fs_mesh.sc kastSdfgiDiffuse / kastSdfgiSpecular, in its SDFGI variant fs_mesh_sdfgi: no .kshader takes
//    it): a mesh without a lightmap or VoxelGI volume (lightmap.h selectDiffuseGi) samples the finest cascade holding
//    it: the 8 probes around the point (pushed off the surface along its normal), trilinear weights times a backface
//    weight (sdfgiProbeWeights), invalid probes skipped, blended into the next cascade over the last kSdfgiBlendProbes
//    probes and into the light without SDFGI past the last. Rough surfaces (roughness from kSdfgiRoughness - 0.1 to
//    + 0.1) take the probes' radiance along the reflection instead of the sky's specular; a ReflectionProbe's box keeps
//    its own.
// Dynamic meshes receive it but never enter the SDF: they neither bounce nor block its light (its shadows are the SDF's,
// not the shadow maps').
// ponytail: static models enter the SDF as their submeshes' world bounds (solid boxes); voxelize their triangles if
// hollow or concave models must hold light. Albedo is the material's base color factor (textures ignored).
namespace kast {

constexpr int kSdfgiCells = 64;      // cells per cascade along x and z (y: times yScale)
constexpr int kSdfgiProbeCells = 4;  // cells between probes; the step a cascade scrolls by
constexpr float kSdfgiMaxDistance = 4.0f;  // cells: the SDF stores distances clamped to this
constexpr float kSdfgiBlendProbes = 2.0f;  // probes over which a cascade fades into the next at its edge
constexpr float kSdfgiRoughness = 0.5f;    // reflections from SDFGI above this roughness (blended over +-0.1)
constexpr float kSdfgiNormalBias = 0.25f;  // probe spacings a shaded point is pushed along its normal
constexpr size_t kMaxSdfgiShapes = 8192;   // shape references uploaded per frame at most (the rest wait a frame)

// A box of global cells (a cascade's integer grid of its cell size): [min, max) per axis.
struct CellBox {
    std::array<int, 3> min{0, 0, 0}, max{0, 0, 0};
    bool empty() const { return min[0] >= max[0] || min[1] >= max[1] || min[2] >= max[2]; }
    size_t cells() const
    {
        return empty() ? 0 : size_t(max[0] - min[0]) * size_t(max[1] - min[1]) * size_t(max[2] - min[2]);
    }
    bool contains(const std::array<int, 3>& c) const
    {
        return c[0] >= min[0] && c[0] < max[0] && c[1] >= min[1] && c[1] < max[1] && c[2] >= min[2] && c[2] < max[2];
    }
    bool operator==(const CellBox&) const = default;
};
CellBox intersect(const CellBox& a, const CellBox& b);

// One cascade's placement: dims cells of cellSize meters from global cell origin (a multiple of kSdfgiProbeCells).
struct SdfgiCascade {
    std::array<int, 3> origin{0, 0, 0};
    std::array<int, 3> dims{kSdfgiCells, kSdfgiCells, kSdfgiCells};
    float cellSize = 1.0f;
    CellBox box() const { return {origin, {origin[0] + dims[0], origin[1] + dims[1], origin[2] + dims[2]}}; }
    bx::Vec3 worldMin() const
    {
        return {float(origin[0]) * cellSize, float(origin[1]) * cellSize, float(origin[2]) * cellSize};
    }
    bx::Vec3 worldMax() const
    {
        return {float(origin[0] + dims[0]) * cellSize, float(origin[1] + dims[1]) * cellSize,
                float(origin[2] + dims[2]) * cellSize};
    }
    // The texel of global cell c in this cascade's toroidal textures: c mod dims (non-negative).
    std::array<int, 3> texel(const std::array<int, 3>& c) const;
    // The global cell holding world point p (faces belong to the cell above).
    std::array<int, 3> cellAt(const bx::Vec3& p) const;
    std::array<int, 3> probeDims() const
    {
        return {dims[0] / kSdfgiProbeCells, dims[1] / kSdfgiProbeCells, dims[2] / kSdfgiProbeCells};
    }
    float probeSpacing() const { return cellSize * float(kSdfgiProbeCells); }
    bool operator==(const SdfgiCascade&) const = default;
};
// Cells per axis of every cascade: kSdfgiCells, times yScale in y.
std::array<int, 3> sdfgiDims(float yScale);
// Cascade i (0 first) of the settings, centered on eye: cell size cellSize * 2^i, its origin the eye's cell minus half
// the dims, rounded down to a multiple of kSdfgiProbeCells.
SdfgiCascade sdfgiCascadeAt(const bx::Vec3& eye, int cascade, float cellSize, float yScale);
// The cells of to that from didn't cover: up to three disjoint slabs (x, then y, then z, each without the ones before);
// to's whole box if they don't overlap or differ in cell size or dims.
std::vector<CellBox> sdfgiScrollRegions(const SdfgiCascade& from, const SdfgiCascade& to);
// The cells of cascade within margin cells of world box bounds (clipped to the cascade).
CellBox sdfgiCellsNear(const SdfgiCascade& cascade, const Aabb& bounds, float margin);

// A heightfield's signed distance at height y above a column of the given height and slope (|gradient| of the height):
// the vertical distance scaled by the cosine of the slope, exact for a plane. Negative below the surface.
float sdfgiHeightfieldDistance(float y, float height, float slope);
// A cascade's terrain columns, in its toroidal texel order (x fastest, then z): per column (x, z cell) the ground's
// height at its center and its slope there (central differences a cell apart), from heightAt (nullopt: no terrain there;
// the column gets kSdfgiNoTerrain and slope 0).
constexpr float kSdfgiNoTerrain = -1.0e6f;
using SdfgiHeightAt = std::function<std::optional<float>(float x, float z)>;
std::vector<std::array<float, 2>> sdfgiTerrainColumns(const SdfgiCascade& cascade, const SdfgiHeightAt& heightAt);

// The probes a point samples in a cascade and their weights (fs_mesh.sc kastSdfgiProbes, cs_sdfgi_light.sc): the 8 around
// p + n * kSdfgiNormalBias spacings (base: the global probe of the lowest corner; corner i is base + (i & 1, i >> 1 & 1,
// i >> 2 & 1)), each its trilinear weight times ((dot(n, toward it) + 1) / 2)^2 + 0.05 (probes behind the surface count
// less), invalid ones (valid(i) false) 0, normalized to sum 1 (all 0 if none is valid).
struct SdfgiProbeWeights {
    std::array<int, 3> base{0, 0, 0};
    std::array<float, 8> weights{};
};
SdfgiProbeWeights sdfgiProbeWeights(const SdfgiCascade& cascade, const bx::Vec3& p, const bx::Vec3& n,
                                    const std::function<bool(int corner)>& valid);
// How much a point at p uses cascade (fs_mesh.sc kastSdfgiEdge): 1 kSdfgiBlendProbes probe spacings or more inside its
// outermost probes, falling linearly to 0 at them (and 0 outside).
float sdfgiCascadeWeight(const SdfgiCascade& cascade, const bx::Vec3& p);

// A quality preset: rays per probe per frame, SDF steps per ray at most, and how much of a probe's light is kept each
// frame (the rest is this frame's rays). Low / Medium / High: 16 / 32 / 64 rays, 32 / 48 / 64 steps, 0.95 / 0.92 / 0.9.
struct SdfgiPreset {
    int rays = 32;
    int steps = 48;
    float hysteresis = 0.92f;
};
SdfgiPreset sdfgiPreset(SdfgiQuality quality);
// Why settings can't be used ("cascades must be 1 to 4", ...), or null: the ranges scene_io enforces on load.
const char* validateSdfgi(const SceneSdfgi& settings);

// A Static shape in the SDF (cs_sdfgi_build.sc reads 6 vec4 each): world to its unscaled local frame (3 rows: xyz
// rotation, w translation), its half extents and type, albedo and emission (linear).
enum class SdfgiShapeType { Box = 0, Ellipsoid = 1 };
struct SdfgiShape {
    std::array<std::array<float, 4>, 3> rows{};
    bx::Vec3 half{0.5f, 0.5f, 0.5f};
    SdfgiShapeType type = SdfgiShapeType::Box;
    bx::Vec3 albedo{0.8f, 0.8f, 0.8f};
    bx::Vec3 emission{0.0f, 0.0f, 0.0f};
    Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world
    bool operator==(const SdfgiShape& o) const;
    bool operator<(const SdfgiShape& o) const;
};
// A primitive of local half extents (centered) under world matrix world (its axes' lengths are its scale; shear ignored).
// A plane gets a sliver of thickness (kSdfgiPlaneHalf meters each side).
constexpr float kSdfgiPlaneHalf = 0.05f;
SdfgiShape sdfgiShape(SdfgiShapeType type, const float* world, const bx::Vec3& localHalf, const bx::Vec3& albedo,
                      const bx::Vec3& emission);
// An axis-aligned box over world bounds (a static model's submesh).
SdfgiShape sdfgiBoxShape(const Aabb& bounds, const bx::Vec3& albedo, const bx::Vec3& emission);
// cs_sdfgi_build.sc's distance from p to the shape (exact for boxes, the usual bound for ellipsoids).
float sdfgiShapeDistance(const SdfgiShape& shape, const bx::Vec3& p);

// A streamed terrain tile as SDFGI sees it: its key and world rect (xz), the data it was built from.
struct SdfgiTile {
    int map = 0, tx = 0, tz = 0;
    const void* data = nullptr; // TerrainTileData identity: a reload is a change
    float x0 = 0.0f, z0 = 0.0f, x1 = 0.0f, z1 = 0.0f;
    bool operator<(const SdfgiTile& o) const { return std::tie(map, tx, tz, data) < std::tie(o.map, o.tx, o.tz, o.data); }
    bool operator==(const SdfgiTile& o) const { return std::tie(map, tx, tz, data) == std::tie(o.map, o.tx, o.tz, o.data); }
};

// Turns per-frame logging on (--log-sdfgi): each frame's rebuilt cells and updated probes per cascade.
void setSdfgiLogging(bool on);
bool sdfgiLogging();

// --- the GPU side ---

// The cascades' textures and compute passes (engine/shaders/cs_sdfgi_*.sc), one per Renderer (its camera).
// ponytail: compute and 3D images only (D3D11 has both); without them SDFGI is off (warned) and meshes get the light
// below it in the precedence.
class SdfgiRenderer {
public:
    void init();
    void shutdown();
    bool supported() const { return bgfx::isValid(build_); }

    // What a frame's update reads.
    struct Frame {
        bx::Vec3 eye{0.0f, 0.0f, 0.0f};
        SceneSdfgi settings;
        std::vector<SdfgiShape> shapes;      // the scene's Static shapes this frame
        std::vector<SdfgiTile> tiles;        // the streamed terrain tiles
        SdfgiHeightAt heightAt;              // the tiles' ground (null: no terrain)
        bx::Vec3 terrainAlbedo{0.5f, 0.5f, 0.5f};
        const bx::Vec3* sunDirection = nullptr; // the way it shines; null: no directional light
        bx::Vec3 sunColor{0.0f, 0.0f, 0.0f};
        std::vector<LocalLightSample> lights;
        bool sky = false; // skySh is the sky's (else the flat ambient lights the rays that escape)
        Sh9 skySh{};
        bx::Vec3 ambient{0.0f, 0.0f, 0.0f};
    };
    // Scrolls / rebuilds what changed, relights and updates the probes, in view (named "SDFGI").
    void update(bgfx::ViewId view, const Frame& frame);
    // Drops the cascades (the next update builds them all again).
    void reset();
    bool ready() const { return !cascades_.empty(); }

    // For the mesh draws (fs_mesh.sc u_sdfgi, s_sdfgi on stage 10): binds the probes and sets the uniform; grid: what
    // the mesh gets past the last cascade is the probe grid (u_giSh), specular: rough reflections from SDFGI. Without
    // SDFGI (on: false) the uniform is zeros.
    void bind(bool on, bool grid, bool specular) const;

private:
    void create(const SceneSdfgi& settings);
    void destroyTextures();
    struct Dirty {
        int cascade = 0;
        CellBox box;
        bool resetProbes = false;
    };

    bgfx::ProgramHandle build_ = BGFX_INVALID_HANDLE, light_ = BGFX_INVALID_HANDLE, probe_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cascadesUniform_ = BGFX_INVALID_HANDLE, gridUniform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle buildUniform_ = BGFX_INVALID_HANDLE, paramsUniform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightsUniform_ = BGFX_INVALID_HANDLE, skyUniform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle rotationUniform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle meshUniform_ = BGFX_INVALID_HANDLE, meshSampler_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout_; // one vec4

    // Stacked per cascade along z (cascade c at z c * dims.z): albedo (RGBA8), emission + distance (RGBA16F: rgb, a
    // in cells), light (RGBA16F), terrain columns (RG32F 2D: dims.x x dims.z per cascade, stacked along y), probes
    // (RGBA16F, 4 texels along x per probe: L1 harmonics; ping-pong).
    bgfx::TextureHandle albedo_ = BGFX_INVALID_HANDLE, sdf_ = BGFX_INVALID_HANDLE, lightVolume_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle terrain_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::TextureHandle, 2> probes_{{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}};
    bgfx::DynamicVertexBufferHandle shapes_ = BGFX_INVALID_HANDLE; // the boxes' shapes this frame

    SceneSdfgi settings_;                 // what the textures were made for (but enabled / energy / feedback)
    std::vector<SdfgiCascade> cascades_;  // empty: nothing built
    std::vector<SdfgiShape> shapesLast_; // sorted
    std::vector<SdfgiTile> tilesLast_;   // sorted
    std::vector<Dirty> pending_;          // regions still to rebuild (past kMaxSdfgiShapes)
    size_t current_ = 0;                  // the probe texture the meshes read
    uint32_t frame_ = 0;
    float energy_ = 1.0f;
};

} // namespace kast
