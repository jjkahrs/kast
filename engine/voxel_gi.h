#pragma once

#include "ibl.h"
#include "lightmap_bake.h"
#include "renderer.h"
#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// VoxelGI volumes (docs/engine/voxelgi.html): real-time relit diffuse GI and glossy reflections inside a box, from its
// Static geometry voxelized once at bake time.
//  - Bake (editor, CPU, bakeVoxelGi): the Static meshes and lightmapped Terrain tiles in the box (gathered as Bake
//    Lightmaps gathers them, lightmap_bake.h) are sampled at half a voxel's spacing into a grid of cubic voxels:
//    resolution along the box's longest axis, the others as many as cover it. Each touched voxel keeps its surfaces'
//    mean albedo, normal and emission. Written to <scene-dir>/<scene-name>_voxelgi/<id>.kvox (the voxels) and <id>.json
//    (the sidecar: version, grid, voxel count, the kvox's FNV-1a hash and the hash of what was baked), atomically, json
//    last.
//  - Every frame (GPU compute, VoxelGiRenderer, in its own "VoxelGI" view after the shadow maps): inject lights every
//    voxel with the scene's lights as fs_mesh.sc shades them (voxelGiLightIrradiance; the shadow maps of this frame
//    occlude), plus its emission; the radiance volume (RGBA16F, rgb premultiplied by opacity in a) gets its mips
//    (isotropic: one 3D texture, 2x2x2 box filter); each bounce gathers the mipped volume with the preset's diffuse cones
//    at every voxel and writes direct + albedo * gathered into the other volume (ping-pong), mipped again.
//  - Shading (fs_mesh.sc kastGiDiffuse, kastVoxelCone): a mesh whose world bounds' center is inside the box (the
//    selection rule is selectProbe's: priority, then the nearest box center) cone-traces the volume: its diffuse from
//    the preset's cones around the normal, its specular from one cone along the reflection as wide as its roughness;
//    what a cone leaves the box with unblocked sees the environment (sky, probe or flat ambient). Both blend into what
//    the mesh would get without the volume over the box's last kVoxelGiEdgeVoxels voxels (voxelGiEdgeWeight). A cone
//    sample's opacity is scaled by its mip voxel's size in voxels (a voxel-thin wall fills 1 / 2^mip of it: so it blocks
//    as a solid one would) and the sample is lifted off the surface by the cone's width (so the surface's own voxels,
//    in every coarse mip voxel around it, don't shade it).
// Dynamic meshes receive it but are never voxelized: they neither bounce nor block its light (their shadows in the
// shadow maps do darken what inject lights).
// ponytail: isotropic mips (one texture, 1/7 more memory, light leaks through walls thinner than ~2 voxels at coarse
// mips); six anisotropic volumes if thin walls must hold.
namespace kast {

constexpr int kVoxelGiFormatVersion = 1;
constexpr int kVoxelGiResolutions[] = {64, 128, 256}; // VoxelGI::resolution's choices
constexpr int kVoxelGiEdgeVoxels = 4;      // voxels over which a volume's light fades out at its box's faces
constexpr size_t kMaxVoxelGiLights = 16;   // MAX_LIGHTS in cs_voxelgi_inject.sc: the nearest the volume's center
constexpr int kMaxVoxelGiCones = 9;        // MAX_CONES in fs_mesh.sc / cs_voxelgi_bounce.sc
constexpr uint32_t kMaxVoxelGiVoxels = 65535u * 64u; // a bake's voxels at most: one dispatch's threads
constexpr float kVoxelGiMinAperture = 0.04f; // a specular cone's narrowest tan(half angle): mirror reflections

// A volume's voxel grid in world space: dims voxels of voxelSize meters from origin (its min corner), x fastest.
struct VoxelGrid {
    bx::Vec3 origin{0.0f, 0.0f, 0.0f};
    float voxelSize = 1.0f;
    std::array<int, 3> dims{0, 0, 0};
    size_t count() const { return size_t(dims[0]) * size_t(dims[1]) * size_t(dims[2]); }
    bx::Vec3 extent() const { return bx::mul(bx::Vec3{float(dims[0]), float(dims[1]), float(dims[2])}, voxelSize); }
    bx::Vec3 center() const { return bx::add(origin, bx::mul(extent(), 0.5f)); }
    size_t index(int i, int j, int k) const { return (size_t(k) * size_t(dims[1]) + size_t(j)) * size_t(dims[0]) + size_t(i); }
    std::array<int, 3> coord(size_t index) const;
    bx::Vec3 voxelCenter(int i, int j, int k) const;
    // The voxel containing p (faces belong to the voxel above them; the grid's max faces to none); nullopt outside.
    std::optional<std::array<int, 3>> voxelAt(const bx::Vec3& p) const;
    bool operator==(const VoxelGrid& other) const
    {
        return origin.x == other.origin.x && origin.y == other.origin.y && origin.z == other.origin.z &&
               voxelSize == other.voxelSize && dims == other.dims;
    }
};
// The grid of a VoxelGI volume centered at center: voxels of longest(boxSize) / resolution, as many per axis as cover
// boxSize (at least one), centered on center.
VoxelGrid voxelGridFor(const bx::Vec3& center, const bx::Vec3& boxSize, int resolution);
// Levels of the radiance volume's mip chain: down to 1 voxel on the longest axis.
uint8_t voxelGridMips(const VoxelGrid& grid);
// The box a mesh selects the volume by (selectProbe, as reflection probes): the grid's, with its priority.
ProbeBox voxelGiBox(const VoxelGrid& grid, int priority);
// fs_mesh.sc kastVoxelEdge: 1 deeper than kVoxelGiEdgeVoxels voxels inside the grid, falling linearly to 0 at its faces
// (and outside).
float voxelGiEdgeWeight(const VoxelGrid& grid, const bx::Vec3& p);

// A quality preset: diffuse cones (each aperture wide: tan of its half angle) per pixel and per voxel in a bounce, each
// step stepScale times the cone's diameter there (itself at least a voxel), bounces after the direct light, and the
// specular and each diffuse cone's steps at most. Low / Medium / High: 4 / 6 / 9 cones, steps of 1 / 0.75 / 0.5
// diameters, 1 / 1 / 2 bounces, 32 / 64 / 128 specular steps.
struct VoxelGiPreset {
    int cones = 6;
    float aperture = 0.57735f;
    float stepScale = 0.75f;
    int bounces = 1;
    int specularSteps = 64;
    int diffuseSteps = 16;
};
VoxelGiPreset voxelGiPreset(VoxelGiQuality quality);
// The preset's diffuse cones in the surface's tangent frame (z along the normal), unit, w their weight: one along the
// normal, the rest in a ring 60 degrees off it, weighted by their cosine, summing to 1 (so a uniform radiance L gathers
// L: irradiance / pi, as u_envSh). As fs_mesh.sc and cs_voxelgi_bounce.sc read u_voxelCones.
std::vector<std::array<float, 4>> voxelGiCones(int count);
// One step of a cone march (fs_mesh.sc kastVoxelCone): the distance along the cone (meters), the cone's diameter there
// (at least a voxel) and the mip it samples (log2 of the diameter in voxels).
struct ConeStep {
    float distance = 0.0f, diameter = 0.0f, lod = 0.0f;
};
// A cone of aperture from its apex: the first step a voxel out, each next one stepScale times the diameter further (at
// least a voxel), until maxDistance or maxSteps steps.
std::vector<ConeStep> voxelConeSteps(float aperture, float voxelSize, float stepScale, float maxDistance, int maxSteps);
// fs_mesh.sc's specular cone aperture for a roughness (0..1): tan of half the GGX lobe's width, at least
// kVoxelGiMinAperture.
float voxelGiSpecularAperture(float roughness);

// The lights inject relights a volume with, packed as cs_voxelgi_inject.sc reads u_voxelLights: per light 4 vec4:
//   [0] xyz: position (directional: the direction it shines along), w: type (VoxelGiLightType)
//   [1] rgb: color * intensity, w: range
//   [2] spot: xyz its direction, w cos(outer); area: xyz half its width (along its X), w: x of half its height
//   [3] spot: x cos(inner); area: xy: yz of half its height; z: shadow (-1 none, point cube 0..3, spot / area tile 4..7,
//       8 the directional light's cascades), w: bias (meters)
enum class VoxelGiLightType { Point = 0, Spot = 1, Area = 2, AreaTwoSided = 3, Directional = 4 };
struct VoxelGiLights {
    std::array<std::array<float, 4>, kMaxVoxelGiLights * 4> data{};
    uint32_t count = 0;
};
// shadows: per light the renderer's shadow slot as above (-1 none). The directional light first (if any: color not
// black), then the local lights reaching bounds (lightReaches) nearest its center, at most kMaxVoxelGiLights in all.
VoxelGiLights packVoxelGiLights(const bx::Vec3* sunDirection, const bx::Vec3& sunColor, bool sunShadowed,
                                const std::vector<LocalLightSample>& lights, const std::vector<float>& shadows,
                                const Aabb& bounds);
// cs_voxelgi_inject.sc's light at p (unit normal n), unshadowed: irradiance / pi, as fs_mesh.sc shades a rough white
// surface with it (pointLightFalloff, spotConeFalloff, rectFormFactor and the range window; bakedLightIrradiance).
bx::Vec3 voxelGiLightIrradiance(const VoxelGiLights& lights, size_t light, const bx::Vec3& p, const bx::Vec3& n);

// One voxel of a bake: its grid index, mean linear albedo and opacity (0..1), unit normal and emitted radiance.
struct Voxel {
    uint32_t index = 0;
    bx::Vec3 albedo{0.0f, 0.0f, 0.0f};
    float opacity = 1.0f;
    bx::Vec3 normal{0.0f, 1.0f, 0.0f};
    bx::Vec3 emission{0.0f, 0.0f, 0.0f};
};
// <id>.kvox: "KVOX", u32 version, u32 dims[3], u32 count, then count voxels by ascending index of 16 bytes each: u32
// index, u32 albedo RGBA8 (a: opacity), u32 normal RGB8 (* 0.5 + 0.5), u32 emission RGBE8 (shared exponent), all
// little-endian. ~16 bytes per surface voxel: a 256^3 volume of a 10 m room's surfaces is a few MB, not 64 MB dense.
// And back: false + error unless well-formed with these dims, indices ascending and inside them.
std::string writeVoxels(const std::array<int, 3>& dims, const std::vector<Voxel>& voxels);
bool readVoxels(std::string_view bytes, const std::array<int, 3>& dims, std::vector<Voxel>& out, std::string& error);

// <id>.json.
struct VoxelGiSidecar {
    std::string hash; // voxelGiSceneHash of what was baked
    int resolution = 0;
    VoxelGrid grid;
    uint32_t voxels = 0;
    std::string file;  // the .kvox beside it
    uint64_t fileHash = 0;
};
std::string voxelGiSidecarJson(const VoxelGiSidecar& sidecar);
// False + error ("... is version 99, expected 1 (bake again)", "... has a malformed ...") unless well-formed.
bool parseVoxelGiSidecar(std::string_view text, VoxelGiSidecar& out, std::string& error);
// A baked volume's files as read (json: the sidecar's text, kvox: the file it names) checked against each other and the
// volume's resolution: false + error (its status) if they don't match.
bool checkVoxelGiBake(std::string_view json, std::string_view kvox, int resolution, VoxelGiSidecar& sidecar,
                      std::vector<Voxel>& voxels, std::string& error);

// Where Bake writes volume id of the scene at project-relative scenePath: <scene-dir>/<scene-name>_voxelgi/<id>.json
// (the .kvox beside it, same stem).
std::string voxelGiSidecarPath(const std::string& scenePath, EntityId id);

// 16 hex digits over what a volume's bake depends on: its grid (position, box size, resolution), every Static mesh
// (id, mesh, material, world matrix) and lightmapped Terrain. Differs: the status is Out of date.
// ponytail: every Static mesh of the scene, not only those in the box (moving one elsewhere outdates every volume).
std::string voxelGiSceneHash(const Scene& scene, entt::entity volume);

// --- the bake ---

struct VoxelGiBakeInput {
    EntityId id = 0;
    std::string name;
    int resolution = 128;
    VoxelGrid grid;
    std::string hash;               // voxelGiSceneHash
    LightmapBakeInput scene;        // its surfaces, Static entities and lightmapped terrain (gatherLightmapBake)
    std::filesystem::path json;     // where the sidecar goes, absolute
    std::string bakedPath;          // the same, project-relative
};
// Volume (a VoxelGI entity of the scene saved at project-relative scenePath under root) as bake input. False + error if
// it can't be (no project path). A scene without Static geometry bakes an empty volume (cones see the environment).
bool gatherVoxelGiBake(const Scene& scene, const std::filesystem::path& root, const std::string& scenePath,
                       entt::entity volume, VoxelGiBakeInput& out, std::string& error);
// Voxelizes input's triangles into its grid: every triangle sampled at half a voxel (or less) apart, each sample adding
// its surface's albedo, emission and interpolated normal to the voxel it falls in. Deterministic.
std::vector<Voxel> voxelizeVoxelGi(const VoxelGiBakeInput& input, LightmapBakeProgress& progress);
struct VoxelGiBakeOutput {
    std::vector<std::pair<std::filesystem::path, std::string>> files; // the .kvox, then the .json
    size_t voxels = 0;
    double seconds = 0.0;
};
// Voxelizes and encodes input (on the calling thread). False + error ("cancelled" if progress.cancel was set).
bool bakeVoxelGi(const VoxelGiBakeInput& input, LightmapBakeProgress& progress, VoxelGiBakeOutput& out, std::string& error);

// --- the GPU side ---

// The compute passes (engine/shaders/cs_voxelgi_*.sc) that relight baked volumes every frame.
// ponytail: compute and 3D textures only (D3D11 has both); without them VoxelGI is off (warned) and meshes get the
// light below it in the precedence.
class VoxelGiRenderer {
public:
    void init();
    void shutdown();
    bool supported() const { return bgfx::isValid(inject_); }

    // A baked volume on the GPU: its voxels (4 vec4 each: (x, y, z, opacity), (albedo, 0), (normal, 0), (emission, 0)),
    // their direct light (1 vec4 each, rewritten every frame) and two radiance volumes with their mips.
    struct Volume {
        VoxelGrid grid;
        uint32_t voxels = 0;
        uint8_t mips = 1;
        bgfx::VertexBufferHandle data = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle direct = BGFX_INVALID_HANDLE;
        std::array<bgfx::TextureHandle, 2> radiance{{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}};
    };
    // False (nothing made) if unsupported.
    bool create(Volume& volume, const VoxelGrid& grid, const std::vector<Voxel>& voxels);
    void destroy(Volume& volume);

    // This frame's shadow maps as fs_mesh.sc samples them (renderer.cpp ShadowFrame): the atlas and its matrices, the
    // cascades' biases and count (0: the directional light casts none), the point light cubes.
    struct Shadows {
        bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
        const float* csmMtx = nullptr; // (kMaxShadowCascades + kMaxShadowedLocalLights) matrices
        const float* csmBias = nullptr; // kMaxShadowCascades vec4
        int cascades = 0;
        std::array<bgfx::TextureHandle, kMaxShadowedPointLights> cubes{
            {{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}}; // invalid: none
    };
    // Relights volume into view: inject, mips, preset.bounces bounces (each mipped). Returns the radiance volume the
    // meshes then sample (its final ping-pong half).
    bgfx::TextureHandle update(bgfx::ViewId view, const Volume& volume, const VoxelGiPreset& preset,
                               const VoxelGiLights& lights, const Shadows& shadows);

private:
    void mips(bgfx::ViewId view, const Volume& volume, bgfx::TextureHandle texture);

    bgfx::ProgramHandle inject_ = BGFX_INVALID_HANDLE, mip_ = BGFX_INVALID_HANDLE, bounce_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle params_ = BGFX_INVALID_HANDLE, lights_ = BGFX_INVALID_HANDLE, cones_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmMtx_ = BGFX_INVALID_HANDLE, csmBias_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSampler_ = BGFX_INVALID_HANDLE, sourceSampler_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::UniformHandle, kMaxShadowedPointLights> cubeSamplers_{};
    bgfx::TextureHandle noShadow2D_ = BGFX_INVALID_HANDLE, noShadowCube_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout_; // one vec4
};

} // namespace kast
