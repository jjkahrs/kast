#include "frustum.h"

namespace kast {

Frustum frustumFromViewProj(const float* viewProj, bool homogeneousDepth)
{
    // Clip = (p, 1) * viewProj (bx row vectors), so clip component j is the dot with column j (Gribb & Hartmann).
    const auto column = [&](int j) {
        return std::array<float, 4>{viewProj[j], viewProj[4 + j], viewProj[8 + j], viewProj[12 + j]};
    };
    const std::array<float, 4> x = column(0), y = column(1), z = column(2), w = column(3);
    const auto plane = [](const std::array<float, 4>& a, const std::array<float, 4>& b, float sign) {
        const bx::Vec3 normal{a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2]};
        const float length = bx::length(normal);
        return Frustum::Plane{bx::mul(normal, 1.0f / length), (a[3] + sign * b[3]) / length};
    };
    const std::array<float, 4> zero{0.0f, 0.0f, 0.0f, 0.0f};
    Frustum frustum{{
        plane(w, x, 1.0f),  // -w <= x
        plane(w, x, -1.0f), // x <= w
        plane(w, y, 1.0f),
        plane(w, y, -1.0f),
        homogeneousDepth ? plane(w, z, 1.0f) : plane(z, zero, 1.0f), // -w <= z, or 0 <= z
        plane(w, z, -1.0f), // z <= w
    }};
    return frustum;
}

bool intersects(const Frustum& frustum, const Aabb& box)
{
    for (const Frustum::Plane& plane : frustum.planes) {
        // The box corner furthest along the plane's normal: if even it is outside, the whole box is.
        const bx::Vec3 corner{plane.normal.x >= 0.0f ? box.max.x : box.min.x, plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                              plane.normal.z >= 0.0f ? box.max.z : box.min.z};
        if (bx::dot(plane.normal, corner) + plane.distance < 0.0f)
            return false;
    }
    return true;
}

} // namespace kast
