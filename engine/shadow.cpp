#include "shadow.h"

#include <algorithm>
#include <cmath>

namespace kast {

namespace {
constexpr float kCasterMargin = 1.0f; // meters past the nearest caster, so casters on the near plane aren't clipped
}

std::array<float, kMaxShadowCascades + 1> cascadeSplits(float nearDepth, float farDepth, int count)
{
    count = std::clamp(count, 1, kMaxShadowCascades);
    std::array<float, kMaxShadowCascades + 1> splits;
    splits.fill(farDepth);
    splits[0] = nearDepth;
    for (int i = 1; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float logarithmic = nearDepth * std::pow(farDepth / nearDepth, t);
        const float uniform = nearDepth + (farDepth - nearDepth) * t;
        splits[static_cast<size_t>(i)] = kCascadeSplitLambda * logarithmic + (1.0f - kCascadeSplitLambda) * uniform;
    }
    return splits;
}

std::array<bx::Vec3, 8> frustumSliceCorners(const float* viewMtx, const float* projMtx, float nearDepth, float farDepth)
{
    float invProj[16], cameraWorld[16];
    bx::mtxInverse(invProj, projMtx);
    bx::mtxInverse(cameraWorld, viewMtx);
    const bx::Vec3 zero{0.0f, 0.0f, 0.0f};
    std::array<bx::Vec3, 8> corners{zero, zero, zero, zero, zero, zero, zero, zero}; // bx::Vec3 has no default
    const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const bool ortho = projMtx[15] != 0.0f;
    for (int i = 0; i < 4; ++i) {
        // A point on the corner's ray (NDC z 1 is the far plane in both depth conventions), then moved to each depth:
        // view space looks down -Z, so its depth is -z. Perspective rays spread from the eye; orthographic ones are
        // parallel.
        const bx::Vec3 ray = bx::mulH({signs[i][0], signs[i][1], 1.0f}, invProj);
        const auto at = [&](float depth) { return ortho ? bx::Vec3{ray.x, ray.y, -depth} : bx::mul(ray, depth / -ray.z); };
        corners[static_cast<size_t>(i)] = bx::mul(at(nearDepth), cameraWorld);
        corners[static_cast<size_t>(i + 4)] = bx::mul(at(farDepth), cameraWorld);
    }
    return corners;
}

Cascade fitCascade(const std::array<bx::Vec3, 8>& corners, const bx::Vec3& lightDir, float casterNear,
                   bool homogeneousDepth)
{
    bx::Vec3 center{0.0f, 0.0f, 0.0f};
    for (const bx::Vec3& corner : corners)
        center = bx::add(center, bx::mul(corner, 1.0f / 8.0f));
    float radius = 0.0f;
    for (const bx::Vec3& corner : corners)
        radius = std::max(radius, bx::length(bx::sub(corner, center)));
    radius = std::ceil(radius * 16.0f) / 16.0f; // a steady size while float noise moves the corners

    Cascade cascade;
    cascade.texelSize = 2.0f * radius / kCascadeSize;
    // Rotation only (eye at the origin), so snapping the center in light space snaps the whole projection.
    const bx::Vec3 up = std::abs(lightDir.y) > 0.99f ? bx::Vec3{0.0f, 0.0f, 1.0f} : bx::Vec3{0.0f, 1.0f, 0.0f};
    bx::mtxLookAt(cascade.view.data(), {0.0f, 0.0f, 0.0f}, lightDir, up, bx::Handedness::Right);
    bx::Vec3 c = bx::mul(center, cascade.view.data());
    c.x = std::floor(c.x / cascade.texelSize) * cascade.texelSize;
    c.y = std::floor(c.y / cascade.texelSize) * cascade.texelSize;
    // Right-handed light view looks down -Z: a point's distance along the light is -z = dot(p, lightDir).
    const float nearPlane = std::min(-c.z - radius, casterNear) - kCasterMargin;
    const float farPlane = -c.z + radius;
    cascade.depthRange = farPlane - nearPlane;
    bx::mtxOrtho(cascade.proj.data(), c.x - radius, c.x + radius, c.y - radius, c.y + radius, nearPlane, farPlane, 0.0f,
                 homogeneousDepth, bx::Handedness::Right);
    return cascade;
}

} // namespace kast
