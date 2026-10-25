#pragma once

#include "math3d.h"
#include "scene.h"
#include "voxel_gi.h" // packVoxelGiLights, VoxelGiRenderer::Shadows

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <vector>

namespace kast {

// The froxel grid (FROXELS_* in shaders/volumetric_fog.sh): kFroxelsX x kFroxelsY cells across the camera's view, by
// kFroxelSlices depth slices spaced exponentially from kFroxelNear to the fog's maxDistance.
constexpr int kFroxelsX = 160, kFroxelsY = 90, kFroxelSlices = 64;
constexpr float kFroxelNear = 0.5f; // meters
constexpr size_t kMaxFogVolumes = 8; // MAX_FOG_VOLUMES in cs_volfog_inject.sc: the FogVolumes nearest the eye

// CPU mirrors of the volumetric fog shaders' math, for self_check. Keep in sync with the shaders named.
// Henyey-Greenstein phase (volumetric_fog.sh kastHenyeyGreenstein): (1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^1.5), per
// steradian; cosTheta between the light's travel and the direction scattered toward the eye.
float henyeyGreenstein(float cosTheta, float g);
// The exponential slice mapping (volumetric_fog.sh kastFroxelDepth / kastFroxelSlice): slice s (0..kFroxelSlices,
// continuous) is at view depth kFroxelNear * (maxDistance / kFroxelNear)^(s / kFroxelSlices); froxelSlice inverts it.
float froxelSliceDepth(float slice, float maxDistance);
float froxelSlice(float depth, float maxDistance);

// A FogVolume as the inject pass reads it (cs_volfog_inject.sc u_fogVolume*).
struct FogVolumeSample {
    std::array<float, 16> worldToLocal{}; // its unit shape's space: the box / sphere spans -0.5..0.5
    FogVolumeShape shape = FogVolumeShape::Box;
    float density = 0.0f, edgeFalloff = 0.0f, heightFalloff = 0.0f;
    bx::Vec3 albedo{1.0f, 1.0f, 1.0f};
    float bottom = 0.0f; // world y of its lowest point, where heightFalloff starts
    Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world
};
FogVolumeSample fogVolumeSample(const FogVolume& volume, const float* world);
// Its extinction per meter at world point p (cs_volfog_inject.sc kastFogVolume): density inside, fading to 0 over the
// last edgeFalloff of the way from its center to its surface (d: 0 center, 1 surface, by the box's largest axis or the
// sphere's radius), times exp(-heightFalloff * height above its bottom); 0 outside.
float fogVolumeDensity(const FogVolumeSample& volume, const bx::Vec3& p);

// Froxel volumetric fog (docs/engine/volumetric-fog.html), one per Renderer (its camera's history). update() runs three
// compute passes in view: inject (each froxel's extinction and scattering: the scene's height fog plus FogVolumes), the
// in-scattering of the sun (through its cascades), the local lights (shadow maps where they cast shadows; area lights
// are left out) and the ambient, reprojected onto the last frame's by the camera's motion and blended by temporalBlend,
// then integrated front to back into a 2D atlas (slice i of kFroxelsX columns at x i * kFroxelsX: what the eye sees of
// fog up to slice i's far side, rgb in-scattered light, a transmittance). Then in view + 1 it composites that over the
// opaque scene by its depth: color * transmittance + in-scattered. Blended meshes and particles sample the atlas
// themselves (drawParams, texture). RGBA16F: density, scattering and its history 3D, 160 x 90 x 64 (7 MiB each), the
// atlas 10240 x 90 (7 MiB); made on first use, freed by release().
// Each froxel is jittered within itself every frame while temporalBlend > 0; at 0 (no history) it is lit at 4 points
// instead. Every lookup is dithered per pixel by up to half a froxel, and the composite by 1/255, so neither the
// grid's steps nor 8-bit banding show.
// ponytail: a fixed grid and camera-only reprojection: moving lights and objects leave a short trail at a high
// temporalBlend; per-object motion or a variance clamp of the history if that shows.
class VolumetricFog {
public:
    struct Frame {
        const ScenePostProcessing::VolumetricFog& settings;
        const float* viewMtx;
        const float* projMtx;
        const bx::Vec3* sunDirection; // null: no DirectionalLight
        bx::Vec3 sunColor;
        bool sunShadowed;
        const std::vector<LocalLightSample>& lights;
        const std::vector<float>& shadows; // per light, the renderer's shadow slot (-1 none), as packVoxelGiLights
        VoxelGiRenderer::Shadows maps;
        bx::Vec3 ambient; // the scene's ambient color * intensity
        std::vector<FogVolumeSample> volumes;
    };

    void init();
    void shutdown();
    // A GPU with compute and RGBA16F 3D images.
    bool supported() const { return bgfx::isValid(inject_); }
    // Runs the passes in view and the composite over target's color (its depth sampled; width x height) in view + 1.
    // False (nothing submitted) when the fog adds nothing: density 0 and no FogVolume of density > 0.
    bool update(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height, const Frame& frame);
    // Frees the volumes and forgets the history (the fog is off this frame).
    void release();
    // After an update() that returned true: u_volFog (fs_mesh.sc, fs_particle.sc; [0] xyz the camera's forward, w 1;
    // [1] y maxDistance) and the atlas they sample.
    const std::array<std::array<float, 4>, 2>& drawParams() const { return drawParams_; }
    bgfx::TextureHandle texture() const { return integrated_; }

private:
    bgfx::ProgramHandle inject_ = BGFX_INVALID_HANDLE, scatter_ = BGFX_INVALID_HANDLE, integrate_ = BGFX_INVALID_HANDLE,
                        composite_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle grid_ = BGFX_INVALID_HANDLE, prev_ = BGFX_INVALID_HANDLE, lights_ = BGFX_INVALID_HANDLE,
                        volumeMtx_ = BGFX_INVALID_HANDLE, volumes_ = BGFX_INVALID_HANDLE, csmMtx_ = BGFX_INVALID_HANDLE,
                        csmBias_ = BGFX_INVALID_HANDLE, csmSampler_ = BGFX_INVALID_HANDLE,
                        historySampler_ = BGFX_INVALID_HANDLE, drawUniform_ = BGFX_INVALID_HANDLE,
                        tex0_ = BGFX_INVALID_HANDLE, tex1_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::UniformHandle, kMaxShadowedPointLights> cubeSamplers_{};
    bgfx::TextureHandle noShadow2D_ = BGFX_INVALID_HANDLE, noShadowCube_ = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle triangle_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle density_ = BGFX_INVALID_HANDLE, integrated_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::TextureHandle, 2> scattering_{{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}}; // ping-pong history
    // target's color alone (owns none), for the composite: remade when its texture or size changes.
    bgfx::FrameBufferHandle color_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle colorTexture_ = BGFX_INVALID_HANDLE;
    uint16_t colorWidth_ = 0, colorHeight_ = 0;
    // The last frame's, for the reprojection: valid only right after an update() at the same maxDistance.
    bool history_ = false;
    float historyDistance_ = 0.0f;
    std::array<float, 16> prevViewProj_{};
    bx::Vec3 prevEye_{0.0f, 0.0f, 0.0f}, prevForward_{0.0f, 0.0f, -1.0f};
    uint32_t frame_ = 0; // updates, for the jitter
    size_t current_ = 0; // scattering_ written last
    std::array<std::array<float, 4>, 2> drawParams_{};
};

} // namespace kast
