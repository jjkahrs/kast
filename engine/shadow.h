#pragma once

#include "math3d.h"
#include "model.h" // Mat4
#include "scene.h"

#include <array>
#include <cstdint>

// Shadow map math for the Renderer, GPU-free so self_check can test it. See docs/engine/coordinates-and-scenes.html (Lighting).
namespace kast {

// The shadow atlas: 2 x 4 tiles of kCascadeSize, the directional light's cascades in rows 0..1 (cascade i at column
// i % 2, row i / 2), then the spot / area light tiles in rows 2..3 (tile i at column i % 2, row 2 + i / 2).
constexpr uint16_t kCascadeSize = 2048;   // texels per side of one atlas tile: a cascade, or a spot / area light's map
constexpr uint16_t kShadowAtlasWidth = 2 * kCascadeSize;  // CSM_TEXEL in fs_mesh.sc
constexpr uint16_t kShadowAtlasHeight = 4 * kCascadeSize;
constexpr uint16_t kPointShadowSize = 1024; // texels per side of one point light cube face
constexpr float kPointShadowNear = 0.05f;  // meters; casters closer to a point, spot or area light than this don't shadow
// ponytail: an area light's map is one perspective view from its center (AREA_SHADOW_TAN in fs_mesh.sc): an
// approximation, its shadows don't soften with the rect's size. Several samples over the rect if that matters.
constexpr float kAreaShadowFovDegrees = 120.0f;
// cascadeSplits mixes logarithmic (lambda = 1, tight near the eye) and uniform (0) cascade splits by this.
constexpr float kCascadeSplitLambda = 0.75f;

// View depths (meters along the view direction) bounding count cascades between near and far: [0] = near,
// [i + 1] = far end of cascade i, entries past count = far.
std::array<float, kMaxShadowCascades + 1> cascadeSplits(float nearDepth, float farDepth, int count);

// World-space corners of the camera frustum (perspective or orthographic viewMtx / projMtx, any depth convention)
// between view depths nearDepth and farDepth: near plane corners first.
std::array<bx::Vec3, 8> frustumSliceCorners(const float* viewMtx, const float* projMtx, float nearDepth, float farDepth);

// A directional light's view and orthographic projection for one cascade.
struct Cascade {
    Mat4 view{};
    Mat4 proj{};
    float texelSize = 0.0f;  // meters per shadow map texel
    float depthRange = 0.0f; // meters between the projection's near and far planes
};

// Fits a cascade around corners: a bounding sphere (so its size doesn't change as the camera turns) whose center is
// snapped to whole texels in light space (so shadow edges don't shimmer as the camera moves). lightDir: the
// direction the light travels (normalized). casterNear: the least dot(p, lightDir) of any caster point p, so casters
// between the light and the sphere still cast.
Cascade fitCascade(const std::array<bx::Vec3, 8>& corners, const bx::Vec3& lightDir, float casterNear,
                   bool homogeneousDepth);

} // namespace kast
