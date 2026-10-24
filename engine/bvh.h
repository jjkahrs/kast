#pragma once

#include "math3d.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A bounding volume hierarchy over triangles for the CPU lightmap baker (lightmap_bake.h, docs/engine/lightmaps.html):
// closest-hit and any-hit ray queries. Built once, then read by many threads at once (queries are const). GPU-free.
// ponytail: a binary BVH (binned SAH, up to 4 triangles a leaf) traversed one ray at a time; a 4-wide BVH and ray
// packets (or Embree) if bakes of big scenes get slow.
namespace kast {

// One triangle; id is the caller's (e.g. which mesh and material it belongs to).
struct BvhTriangle {
    bx::Vec3 a{0.0f, 0.0f, 0.0f}, b{0.0f, 0.0f, 0.0f}, c{0.0f, 0.0f, 0.0f};
    uint32_t id = 0;
};

struct BvhHit {
    float t = 0.0f;      // distance along the ray (its direction need not be unit: t is in its units)
    float u = 0.0f, v = 0.0f; // barycentrics: the point is a + u (b - a) + v (c - a)
    uint32_t triangle = 0;    // index into Bvh::triangles() (the order build() was given)
};

class Bvh {
public:
    // Replaces the hierarchy with one over triangles.
    void build(std::vector<BvhTriangle> triangles);
    const std::vector<BvhTriangle>& triangles() const { return triangles_; }
    bool empty() const { return triangles_.empty(); }
    // Bounds of every triangle (all zero when empty).
    Aabb bounds() const;
    // The nearest hit with tMin < t < tMax, either face.
    std::optional<BvhHit> intersect(const bx::Vec3& origin, const bx::Vec3& direction, float tMin, float tMax) const;
    // True if any triangle is hit with tMin < t < tMax (shadow rays).
    bool occluded(const bx::Vec3& origin, const bx::Vec3& direction, float tMin, float tMax) const;

private:
    struct Node {
        bx::Vec3 min{0.0f, 0.0f, 0.0f}, max{0.0f, 0.0f, 0.0f};
        uint32_t first = 0; // leaf: first index in order_; inner: the left child (the right is first + 1)
        uint32_t count = 0; // triangles in a leaf; 0 for an inner node
    };
    // A triangle prepared for Moller-Trumbore: a corner and its two edges.
    struct Prepared {
        bx::Vec3 a{0.0f, 0.0f, 0.0f}, ab{0.0f, 0.0f, 0.0f}, ac{0.0f, 0.0f, 0.0f};
    };
    template <bool AnyHit>
    std::optional<BvhHit> trace(const bx::Vec3& origin, const bx::Vec3& direction, float tMin, float tMax) const;

    std::vector<BvhTriangle> triangles_;
    std::vector<Prepared> prepared_; // by order_ position
    std::vector<uint32_t> order_;    // leaf triangle indices into triangles_
    std::vector<Node> nodes_;
};

// Every triangle tested: the reference Bvh::intersect must match (self_check).
std::optional<BvhHit> intersectBruteForce(std::span<const BvhTriangle> triangles, const bx::Vec3& origin,
                                          const bx::Vec3& direction, float tMin, float tMax);

} // namespace kast
