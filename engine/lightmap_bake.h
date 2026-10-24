#pragma once

#include "bvh.h"
#include "ibl.h"
#include "lightmap.h"
#include "model.h"
#include "renderer.h"
#include "scene.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Bake Lightmaps (docs/engine/lightmaps.html): a CPU path tracer that bakes a scene's Static meshes and lightmapped
// terrain into lightmaps (lightmap.h) and an SH probe grid for its Dynamic meshes. Three steps:
//  1. gatherLightmapBake (main thread): the scene as plain data (meshes, surface colors, lights, sky), so
//  2. bakeLightmaps can run on worker threads while the editor keeps drawing and editing: it unwraps each Static mesh
//     (xatlas; a model's own second UV set, glTF TEXCOORD_1, if it has one) at its texel density, packs them into atlas
//     pages, builds a BVH (bvh.h) over all Static geometry and terrain, traces samplesPerTexel cosine-weighted paths of up
//     to bounces diffuse bounces from every texel (the sky as the environment, emissive surfaces emitting, every light
//     but Realtime ones lighting each bounce, Full lights the texel too), dilates the charts against seams and traces the
//     probe grid. Deterministic: every texel and probe has its own random stream from a fixed seed, whatever the threads.
//  3. writeLightmapBake (main thread): writes what it made, each file atomically, the json last, so a cancelled or
//     failed bake leaves the previous one intact.
// Lights shine as the renderer shades them (pointLightFalloff, spotConeFalloff, rectFormFactor in renderer.h; light
// color * intensity is radiance * pi), so a Full light's baked look matches its realtime one.
namespace kast {

// A surface's diffuse response: albedo (base color times its texture's average, times 1 - metallic) and emitted
// radiance (emissiveRadiance times its texture's average). twoSided: both faces count as front (cull mode None).
struct BakeSurface {
    bx::Vec3 albedo{0.8f, 0.8f, 0.8f};
    bx::Vec3 emission{0.0f, 0.0f, 0.0f};
    bool twoSided = false;
};

// One submesh in its model's space (a primitive's, or a model's bind pose).
struct BakeMesh {
    std::vector<bx::Vec3> positions, normals;
    std::vector<float> uv2; // 2 per vertex: the source's own second UV set (glTF TEXCOORD_1, 0..1); empty: unwrap it
    std::vector<uint32_t> indices;
    uint32_t surface = 0; // into LightmapBakeInput::surfaces
};

// A Static MeshRenderer or ModelRenderer.
struct BakeEntity {
    EntityId id = 0;
    std::string name;
    std::string source; // LightmapMesh::source
    Mat4 world{};
    float density = 8.0f; // lightmap texels per meter: texelsPerMeter * lightmapScale
    std::vector<BakeMesh> meshes;
};

// A light that isn't Realtime, as the renderer sees it (color is color * intensity). A directional light shines along
// sample.direction.
struct BakeLight {
    LocalLightSample sample;
    bool directional = false;
    bool full = false; // Full: also lights the texels directly (else only the bounces)
};

// The radiance escaped rays see: the sky times the ambient (as the renderer tints the sky's light), or with the sky off
// the flat ambient from everywhere.
struct BakeSky {
    enum class Kind { Uniform, Procedural, Cube } kind = Kind::Uniform;
    bx::Vec3 tint{0.2f, 0.2f, 0.2f}; // the ambient color * intensity
    SceneSky sky;                    // Procedural
    CubeSides cube;                  // Cube
    bx::Vec3 radiance(const bx::Vec3& direction) const;
};

// One terrain tile to bake: n x n texels, one per height sample, in world space.
struct BakeTerrainTile {
    int tx = 0, tz = 0;
    std::vector<bx::Vec3> positions, normals; // n * n, row-major along +z
};
// A lightmapped Terrain's map: its tiles, written to folder (maps/<name>/lightmaps, absolute).
struct BakeTerrain {
    std::filesystem::path folder;
    int size = 0; // samples per tile edge
    uint32_t surface = 0;
    std::vector<BakeTerrainTile> tiles;
};

struct LightmapBakeInput {
    SceneLightmaps settings;
    std::string hash; // lightmapSceneHash
    std::vector<BakeSurface> surfaces;
    std::vector<BakeEntity> entities;
    std::vector<BakeLight> lights;
    BakeSky sky;
    std::vector<BakeTerrain> terrain;
    uint64_t seed = 0x6b617374; // "kast"
    std::filesystem::path folder;  // <scene-dir>/<scene-name>_lightmaps, absolute
    std::vector<std::string> warnings; // things gathering left out (logged by the caller)
};

// The scene (saved at project-relative scenePath under root) as bake input. False + error if there is nothing to bake
// (no Static mesh and no lightmapped Terrain). Warnings (a skinned Static model, a model that won't load) go in
// out.warnings.
bool gatherLightmapBake(const Scene& scene, const std::filesystem::path& root, const std::string& scenePath,
                        LightmapBakeInput& out, std::string& error);

// What a bake made: files (absolute path, bytes), each folder's json last; and what it took.
struct LightmapBakeOutput {
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    size_t atlases = 0, texels = 0, probes = 0, terrainTiles = 0;
    double seconds = 0.0;
};
// Shared with the thread running bakeLightmaps: progress 0..1 (it writes), cancel (the caller sets it).
struct LightmapBakeProgress {
    std::atomic<float> fraction{0.0f};
    std::atomic<bool> cancel{false};
};
// Bakes input on threads (std::thread::hardware_concurrency of them). False + error ("cancelled" if progress.cancel was
// set) on failure; out is then untouched.
bool bakeLightmaps(const LightmapBakeInput& input, LightmapBakeProgress& progress, LightmapBakeOutput& out,
                   std::string& error);
// Writes out.files (creating folders, atomically each, in order). False + error naming the first that fails.
bool writeLightmapBake(const LightmapBakeOutput& out, std::string& error);

// --- the pieces, for self_check ---

// Unwraps meshes (one entity's submeshes, scaled per axis by scale) together with xatlas at density texels per unit,
// padding the charts against bilinear bleeding: a width x height texel rect whose submeshes' uv lie in 0..1 of it.
// Meshes that all carry uv2 keep theirs (xref the identity) in a rect sized by their area. False + error if xatlas
// fails.
struct LightmapUnwrap {
    uint16_t width = 0, height = 0;
    std::vector<LightmapSubmesh> submeshes;
};
bool unwrapLightmapMeshes(const std::vector<BakeMesh>& meshes, const bx::Vec3& scale, float density, LightmapUnwrap& out,
                          std::string& error);

// A normal carried by m (inverse transpose: right for non-uniform scale), unit.
bx::Vec3 transformNormal(const bx::Vec3& n, const Mat4& m);

// A light's irradiance / pi at p with unit normal n (lightmap units: a white surface shows it), unshadowed: exactly as
// fs_mesh.sc shades a rough white dielectric with it (pointLightFalloff, spotConeFalloff, rectFormFactor and the range
// window).
bx::Vec3 bakedLightIrradiance(const BakeLight& light, const bx::Vec3& p, const bx::Vec3& n);

// A small deterministic random stream (PCG32): one per texel and probe, from the bake's seed and its index.
class BakeRng {
public:
    BakeRng(uint64_t seed, uint64_t stream);
    uint32_t next();
    float uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); } // 0 <= x < 1

private:
    uint64_t state_ = 0, inc_ = 1;
};

// The scene the paths run through: a BVH over the Static meshes and terrain of an input, its surfaces, lights and sky.
class LightmapTracer {
public:
    explicit LightmapTracer(const LightmapBakeInput& input);
    // Irradiance / pi gathered at p (normal n, unit) from samples cosine-weighted paths of up to bounces bounces, plus the
    // Full lights' direct light if direct; direction: the luminance-weighted mean direction it came from (length <= 1);
    // backfaces: how many first hits were the back of one-sided surfaces (the texel is inside geometry).
    struct Gather {
        bx::Vec3 irradiance{0.0f, 0.0f, 0.0f};
        bx::Vec3 direction{0.0f, 0.0f, 0.0f};
        int backfaces = 0;
    };
    Gather gather(const bx::Vec3& p, const bx::Vec3& n, int samples, int bounces, bool direct, BakeRng& rng) const;
    // Radiance arriving at origin from direction (unit) after up to bounces bounces; backface: the first hit was the back
    // of a one-sided surface (then black).
    bx::Vec3 radiance(const bx::Vec3& origin, const bx::Vec3& direction, int bounces, BakeRng& rng,
                      bool& backface) const;
    const Bvh& bvh() const { return bvh_; }

private:
    bx::Vec3 direct(const bx::Vec3& p, const bx::Vec3& n, bool fullOnly, BakeRng& rng, bx::Vec3* weighted) const;

    const LightmapBakeInput& input_;
    Bvh bvh_;
    std::vector<uint32_t> surfaceOf_; // per triangle
};

} // namespace kast
