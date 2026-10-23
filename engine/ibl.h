#pragma once

#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Image-based lighting (docs/engine/ibl-reflection-probes.html): environment cubes the mesh shader (fs_mesh.sc) lights
// its ambient term with, from the scene's sky and from baked ReflectionProbes. Each environment is a GGX-prefiltered
// cube (s_env: level i is roughness i / (mips - 1)) for specular and order-2 spherical harmonics (u_envSh) for diffuse.
// The math is GPU-free so self_check can test it; EnvironmentFilter runs the prefilter on the GPU.
namespace kast {

constexpr uint16_t kSkyEnvSize = 128;    // texels per side of the sky's environment cube
constexpr uint16_t kEnvSmallestMip = 4;  // the prefiltered chain stops at 4 x 4 (roughness 1)
constexpr uint32_t kEnvSamples = 128;    // GGX samples per texel of each prefiltered level above roughness 0
constexpr uint16_t kProbeResolutions[] = {64, 128, 256}; // ReflectionProbe::resolution's choices

// Levels of a size x size environment cube's prefiltered chain: size down to kEnvSmallestMip (64: 5, 128: 6, 256: 7).
uint8_t envMips(uint16_t size);

// Order-2 spherical harmonics of an environment's radiance, convolved with the clamped cosine and divided by pi, as
// fs_mesh.sc kastSh9 reads them (u_envSh): RGB coefficients c0..c8 (c_i at [3 i .. 3 i + 2]); at unit normal n,
// irradiance / pi = c0 + c1 y + c2 z + c3 x + c4 xy + c5 yz + c6 (3 z^2 - 1) + c7 xz + c8 (x^2 - y^2) (Kast world axes),
// clamped at 0. A uniform environment of radiance L gives L, so a white surface under it shows L, as the flat ambient does.
using Sh9 = std::array<float, 27>;
// A cube's radiance projected: sides of size x size linear RGB float triples (row by row, v down) in bgfx's side order
// and space (cubeTexelDirection).
Sh9 projectSh9(const std::array<std::vector<float>, 6>& sides, uint16_t size);
// fs_mesh.sc kastSh9.
bx::Vec3 shIrradiance(const Sh9& sh, const bx::Vec3& normal);
Sh9 scaleSh9(const Sh9& sh, const bx::Vec3& tint);
// The Kast world direction (not unit) through (u, v) (-1..1 across the face, v down) of cube side 0..5 (+X, -X, +Y,
// -Y, +Z, -Z as bgfx / D3D lay them out; a world direction d is the cube direction (d.x, d.y, -d.z), as for the sky).
bx::Vec3 cubeTexelDirection(int side, float u, float v);
// A procedural sky's environment (proceduralSkyColor in every direction) projected.
Sh9 proceduralSkySh9(const SceneSky& sky);
// A KTX / DDS cube's (any format bimg decodes; srgb: its values are sRGB-encoded) projected, from its first mip at most
// 64 texels across. nullopt if it isn't a cube bimg can read.
std::optional<Sh9> cubeFileSh9(std::string_view bytes, bool srgb);

// fs_mesh.sc kastEnvBrdf: Karis' analytic fit ("Physically Based Shading on Mobile", 2014) of the split-sum's GGX BRDF
// integral at (roughness, N.V): the specular reflectance is F0 * [0] + [1].
std::array<float, 2> envBrdf(float roughness, float nv);

// fs_mesh.sc kastEnvDirection: the direction to sample a box-projected probe's cube with, for reflection ray r from p:
// from the probe's center (the box's) to where r leaves the box (center +- halfSize). r itself if p is outside it.
bx::Vec3 boxProjectedDirection(const bx::Vec3& r, const bx::Vec3& p, const bx::Vec3& center, const bx::Vec3& halfSize);

// A ReflectionProbe in world space, as the renderer selects them.
struct ProbeBox {
    bx::Vec3 center{0.0f, 0.0f, 0.0f};
    bx::Vec3 halfSize{0.5f, 0.5f, 0.5f};
    int priority = 0;
};
// The probe a mesh whose world bounds are centered at point uses: of the boxes containing point (bounds inclusive), the
// highest priority; among equals the nearest box center; then list order. nullopt if no box contains it.
std::optional<size_t> selectProbe(const std::vector<ProbeBox>& probes, const bx::Vec3& point);

// A baked probe: its prefiltered cube as an RGBA16F KTX with envMips(size) levels, and its irradiance.
struct BakedProbe {
    uint16_t size = 0;
    std::string ktx;
    Sh9 sh{};
};
// A baked probe from its prefiltered cube as read back: per level (size >> level texels across, down to 1 x 1) its six
// sides one below the other, RGBA16F texels row by row. The KTX holds them as they are; the harmonics come from level
// 0 (roughness 0: the captured radiance).
BakedProbe makeBakedProbe(uint16_t size, const std::vector<std::vector<uint16_t>>& levels);
// Where Bake writes entity id's probe of the scene at project-relative scenePath: <scene-dir>/<scene-name>_probes/<id>.ktx
// (its SH sidecar beside it, probeShPath).
std::string bakedProbePath(const std::string& scenePath, EntityId id);
// The JSON sidecar holding a baked .ktx's spherical harmonics: the same path, .json instead of .ktx.
std::filesystem::path probeShPath(const std::filesystem::path& ktx);
// Writes the .ktx and its sidecar (atomically, creating the folder). False + error if it can't.
bool writeBakedProbe(const std::filesystem::path& ktx, const BakedProbe& probe, std::string& error);
// The sidecar's text, and back: {"version": 1, "size": <texels>, "sh": [[r, g, b] x 9]}. False + error if malformed.
std::string probeShJson(uint16_t size, const Sh9& sh);
bool parseProbeShJson(std::string_view text, uint16_t& size, Sh9& sh, std::string& error);
// Checks a baked probe's files as read (ktx: its .ktx bytes, json: its sidecar's text) against its resolution: an
// RGBA16F cube of resolution x resolution with envMips(resolution) levels, and a sidecar of that size. False + error
// (the Inspector's status) if not.
bool checkBakedProbe(std::string_view ktx, std::string_view json, uint16_t resolution, Sh9& sh, std::string& error);

// Runs the environment prefilter on the GPU (engine/shaders/cs_ibl.sc compute passes). Every pass of one call goes into
// one view, in order, so the cube is ready for the draws submitted to that view after it.
// ponytail: compute only (D3D11 has it); without BGFX_CAPS_COMPUTE environments are off and the flat ambient stays.
class EnvironmentFilter {
public:
    void init();
    void shutdown();
    bool supported() const { return bgfx::isValid(program_); }
    // A new kSkyEnvSize environment cube of the sky: its gradient, or skyCube (a cube of skyCubeSize, sRGB-sampled) if
    // valid. Invalid if unsupported.
    bgfx::TextureHandle sky(bgfx::ViewId view, const SceneSky& sky, bgfx::TextureHandle skyCube, uint16_t skyCubeSize);
    // A new prefiltered cube (envMips(size) levels, RGBA16F) of source's level 0 (a size x size cube). flags: the new
    // cube's extra flags (BGFX_TEXTURE_BLIT_DST... for reading it back).
    bgfx::TextureHandle prefilter(bgfx::ViewId view, bgfx::TextureHandle source, uint16_t size, uint64_t flags = 0);

private:
    void dispatch(bgfx::ViewId view, bgfx::TextureHandle source, bgfx::TextureHandle target, uint8_t mip, uint16_t size,
                  const float (&params)[4][4]);

    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle params_ = BGFX_INVALID_HANDLE, source_ = BGFX_INVALID_HANDLE;
};

} // namespace kast
