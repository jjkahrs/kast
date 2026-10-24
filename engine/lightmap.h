#pragma once

#include "ibl.h"
#include "scene.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Baked global illumination, the data side (docs/engine/lightmaps.html): what Bake Lightmaps (lightmap_bake.h) writes
// and the renderer reads. GPU-free, so self_check tests it.
//
// A scene's bake lives in <scene-dir>/<scene-name>_lightmaps/ (the scene's lighting.lightmaps.bakedPath names its
// lightmaps.json): per atlas page an RGBA16F irradiance KTX (rgb: irradiance / pi, as u_envSh's harmonics, so a white
// surface shows it) and an RGBA16F direction KTX (xyz: the luminance-weighted mean direction light arrives from, its
// length how directional it is; w: its half-Lambert at the surface's own normal, so fs_mesh.sc can bend the light by a
// normal map and get the stored value back on the flat surface); meshes.klmm, the second UV set of each unwrapped mesh;
// and lightmaps.json: version, the hash of what was baked (out of date when the scene's differs), the atlas pages, each
// Static entity's page and scale / offset into it, and the SH probe grid. Each file's FNV-1a hash is in the json too,
// so a half-written bake reads as mismatched, never as a mix. Terrain tiles bake one lightmap each, under the map's
// folder (maps/<folder>/lightmaps/<tx>_<tz>.ktx and _dir.ktx, listed in its lightmaps.json), so they stream with the
// tile.
namespace kast {

constexpr int kLightmapFormatVersion = 1;
constexpr uint16_t kLightmapAtlasSize = 1024; // texels per side of an atlas page at most
constexpr size_t kMaxGridProbes = 8192;       // the probe grid's spacing grows until it has at most this many

// The diffuse ambient a draw takes (fs_mesh.sc u_gi.x), the one selection point: a lightmap if it has one, else a
// VoxelGI volume if it is in one (voxel_gi.h), else SDFGI if the scene has it on (sdfgi.h), else the probe grid if it
// samples it, else the environment (reflection probe, sky or flat ambient) as before GI.
enum class DiffuseGi { Environment = 0, Lightmap = 1, ProbeGrid = 2, VoxelGi = 3, Sdfgi = 4 };
DiffuseGi selectDiffuseGi(bool lightmapped, bool probeGrid, bool voxelGi = false, bool sdfgi = false);

// Whether a light of this bake mode shines on a draw at run time: everything but a Full light on a lightmapped draw
// (its direct light is in the lightmap already).
bool lightShinesOn(LightBakeMode mode, bool lightmapped);

// fs_mesh.sc kastGiDiffuse's lightmap term: irradiance (rgb / pi) at a texel bent toward normal n by its direction
// texel (xyz, w as above) when directional; irradiance itself on the surface's own normal.
bx::Vec3 lightmapDiffuse(const bx::Vec3& irradiance, const std::array<float, 4>& direction, const bx::Vec3& n,
                         bool directional);

// Order-2 SH probes (Sh9, as u_envSh) on a regular grid: probe (i, j, k) at origin + spacing * (i, j, k). A probe
// buried in geometry (most of its rays hit back faces) is invalid and never sampled.
struct ProbeGrid {
    bx::Vec3 origin{0.0f, 0.0f, 0.0f};
    float spacing = 1.0f;
    std::array<int, 3> counts{0, 0, 0};
    std::vector<Sh9> sh;         // x fastest, then y, then z
    std::vector<uint8_t> valid;  // 1 / 0, per probe
    bool empty() const { return sh.empty(); }
    size_t index(int i, int j, int k) const { return (size_t(k) * counts[1] + size_t(j)) * counts[0] + size_t(i); }
};
// Trilinear at point (clamped into the grid): the eight probes around it weighted by distance, invalid ones left out
// and the rest renormalized (all of them evenly where only invalid ones carry weight, e.g. right on a buried probe).
// nullopt if the grid is empty or all eight are invalid.
std::optional<Sh9> sampleProbeGrid(const ProbeGrid& grid, const bx::Vec3& point);

// One submesh of an unwrapped mesh: per output vertex the source vertex it copies (xref) and its lightmap UV (u, v in
// 0..1 of the mesh's own rect), plus its triangles. sourceVertices: the source submesh's vertex count when unwrapped
// (a model that changed since no longer matches).
struct LightmapSubmesh {
    uint32_t sourceVertices = 0;
    std::vector<uint32_t> xref;
    std::vector<float> uv; // 2 per output vertex
    std::vector<uint32_t> indices;
};
// A mesh as unwrapped once for every Static entity drawing it at one scale and texel density. source: "primitive:cube",
// "primitive:plane", "primitive:sphere" or "model:<project-relative path>"; key: source plus that scale and density.
struct LightmapMesh {
    std::string key;
    std::string source;
    std::vector<LightmapSubmesh> submeshes; // a model's by its mesh index; a primitive's one
};
// meshes as meshes.klmm: "KLMM", u32 version, u32 count, then per mesh: string key, string source, u32 submeshes, then
// per submesh: u32 sourceVertices, u32 V, u32[V] xref, f32[2 V] uv, u32 I, u32[I] indices (strings: u32 length + bytes,
// all little-endian). And back: false + error if malformed.
std::string writeLightmapMeshes(const std::vector<LightmapMesh>& meshes);
bool readLightmapMeshes(std::string_view bytes, std::vector<LightmapMesh>& out, std::string& error);

struct LightmapAtlas {
    uint16_t width = 0, height = 0;
    std::string irradiance, direction; // file names beside lightmaps.json
    uint64_t irradianceHash = 0, directionHash = 0;
};
struct LightmapEntity {
    EntityId id = 0;
    std::string mesh; // LightmapMesh::key
    uint32_t atlas = 0;
    std::array<float, 4> scaleOffset{1.0f, 1.0f, 0.0f, 0.0f}; // atlas uv = mesh uv * xy + zw
};
// lightmaps.json.
struct LightmapSidecar {
    std::string hash; // lightmapSceneHash of what was baked
    std::vector<LightmapAtlas> atlases;
    std::string meshes = "meshes.klmm";
    uint64_t meshesHash = 0;
    std::vector<LightmapEntity> entities;
    ProbeGrid grid;
};
std::string lightmapSidecarJson(const LightmapSidecar& sidecar);
// False + error ("not a version 1 lightmaps.json", "...malformed...") unless the text is a well-formed sidecar of
// kLightmapFormatVersion.
bool parseLightmapSidecar(std::string_view text, LightmapSidecar& out, std::string& error);

// A terrain map's lightmaps.json: the tiles baked, each n x n texels (one per height sample) at <tx>_<tz>.ktx and
// <tx>_<tz>_dir.ktx beside it.
struct TerrainLightmaps {
    std::string hash; // lightmapSceneHash of the scene that baked them
    int size = 0;
    struct Tile {
        int tx = 0, tz = 0;
        uint64_t irradianceHash = 0, directionHash = 0;
    };
    std::vector<Tile> tiles;
};
std::string terrainLightmapsJson(const TerrainLightmaps& index);
bool parseTerrainLightmaps(std::string_view text, TerrainLightmaps& out, std::string& error);
// Checks one tile's two files against the index (its tile and hashes, an RGBA16F 2D KTX of size x size each).
bool checkTerrainLightmap(const TerrainLightmaps& index, int tx, int tz, std::string_view irradiance,
                          std::string_view direction, std::string& error);

// Where Bake Lightmaps writes the scene at project-relative scenePath: <scene-dir>/<scene-name>_lightmaps/lightmaps.json.
std::string lightmapSidecarPath(const std::string& scenePath);
// The map folder's lightmaps folder: "<folder>/lightmaps" (folder: maps/<name>, project-relative or absolute).
std::string terrainLightmapFolder(const std::string& mapFolder);
std::string terrainLightmapFile(int tx, int tz, bool direction); // "<tx>_<tz>.ktx" / "<tx>_<tz>_dir.ktx"

// FNV-1a, 64 bit.
uint64_t lightmapFileHash(std::string_view bytes);
// width x height RGBA float texels as an RGBA16F 2D KTX, and back (false + error unless the bytes are one of that size).
std::string lightmapKtx(uint16_t width, uint16_t height, const std::vector<float>& rgba);
bool checkLightmapKtx(std::string_view bytes, uint16_t width, uint16_t height, std::string& error);

// 16 hex digits over what a bake depends on: every Static mesh (id, mesh, material, world matrix, lightmapScale), every
// light not Realtime (type, bake mode, world matrix, its fields), the ambient and sky, the bake settings and lightmapped
// Terrain. Moving a Static mesh changes it: the Lighting panel then says Out of date.
// ponytail: material and model files are named, not read: editing a .mat's color doesn't mark the bake out of date.
std::string lightmapSceneHash(const Scene& scene);

} // namespace kast
