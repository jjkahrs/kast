#pragma once

#include "math3d.h"

#include <array>

// View frustum culling math for the Renderer, GPU-free so self_check can test it. See docs/engine/instancing-and-culling.html.
namespace kast {

// A convex volume as 6 inward-facing planes: a point p is inside plane i when dot(normal, p) + distance >= 0.
struct Frustum {
    struct Plane {
        bx::Vec3 normal;
        float distance;
    };
    std::array<Plane, 6> planes; // left, right, bottom, top, near, far
};

// The planes of the volume viewProj (view then projection, bx convention) maps to clip space: perspective or
// orthographic. homogeneousDepth: the projection's depth is -1..1 (OpenGL) rather than 0..1 (Direct3D).
Frustum frustumFromViewProj(const float* viewProj, bool homogeneousDepth);

// False only if box lies entirely outside one of the planes. Conservative: a box near a corner of the frustum but
// outside it can still pass, so a caller may draw a little too much, never too little.
bool intersects(const Frustum& frustum, const Aabb& box);

} // namespace kast
