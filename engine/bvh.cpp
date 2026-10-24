#include "bvh.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace kast {
namespace {

constexpr uint32_t kLeafSize = 4; // triangles a leaf may hold
constexpr int kBins = 16;         // SAH split candidates per axis
constexpr int kStackSize = 128;   // traversal depth; binned SAH trees stay far shallower

struct Box {
    bx::Vec3 min{FLT_MAX, FLT_MAX, FLT_MAX}, max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    void grow(const bx::Vec3& p)
    {
        min = bx::min(min, p);
        max = bx::max(max, p);
    }
    void grow(const Box& b)
    {
        min = bx::min(min, b.min);
        max = bx::max(max, b.max);
    }
    float area() const
    {
        const bx::Vec3 d = bx::sub(max, min);
        return d.x < 0.0f ? 0.0f : 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

float axis(const bx::Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

// Moller-Trumbore, both faces: t and barycentrics if the ray hits within (tMin, tMax).
bool hitTriangle(const bx::Vec3& a, const bx::Vec3& ab, const bx::Vec3& ac, const bx::Vec3& origin,
                 const bx::Vec3& direction, float tMin, float tMax, BvhHit& hit)
{
    const bx::Vec3 p = bx::cross(direction, ac);
    const float det = bx::dot(ab, p);
    if (std::abs(det) < 1e-12f)
        return false;
    const float inv = 1.0f / det;
    const bx::Vec3 s = bx::sub(origin, a);
    const float u = bx::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    const bx::Vec3 q = bx::cross(s, ab);
    const float v = bx::dot(direction, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    const float t = bx::dot(ac, q) * inv;
    if (!(t > tMin && t < tMax))
        return false;
    hit.t = t;
    hit.u = u;
    hit.v = v;
    return true;
}

// Slab test: the entry distance, or FLT_MAX on a miss.
float hitBox(const bx::Vec3& min, const bx::Vec3& max, const bx::Vec3& origin, const bx::Vec3& inv, float tMin, float tMax)
{
    const float x0 = (min.x - origin.x) * inv.x, x1 = (max.x - origin.x) * inv.x;
    const float y0 = (min.y - origin.y) * inv.y, y1 = (max.y - origin.y) * inv.y;
    const float z0 = (min.z - origin.z) * inv.z, z1 = (max.z - origin.z) * inv.z;
    const float enter = std::max({std::min(x0, x1), std::min(y0, y1), std::min(z0, z1), tMin});
    const float exit = std::min({std::max(x0, x1), std::max(y0, y1), std::max(z0, z1), tMax});
    return enter <= exit ? enter : FLT_MAX;
}

} // namespace

void Bvh::build(std::vector<BvhTriangle> triangles)
{
    triangles_ = std::move(triangles);
    const auto count = static_cast<uint32_t>(triangles_.size());
    order_.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        order_[i] = i;
    nodes_.clear();
    prepared_.clear();
    if (count == 0)
        return;
    std::vector<Box> boxes(count);
    std::vector<bx::Vec3> centers(count, bx::Vec3{0.0f, 0.0f, 0.0f});
    for (uint32_t i = 0; i < count; ++i) {
        const BvhTriangle& t = triangles_[i];
        boxes[i].grow(t.a);
        boxes[i].grow(t.b);
        boxes[i].grow(t.c);
        centers[i] = bx::mul(bx::add(boxes[i].min, boxes[i].max), 0.5f);
    }
    nodes_.reserve(size_t(count) * 2);
    nodes_.push_back({.first = 0, .count = count});
    std::vector<uint32_t> pending = {0}; // nodes still to bound and maybe split
    while (!pending.empty()) {
        const uint32_t index = pending.back();
        pending.pop_back();
        const uint32_t first = nodes_[index].first, n = nodes_[index].count;
        Box bounds, centroids;
        for (uint32_t i = first; i < first + n; ++i) {
            bounds.grow(boxes[order_[i]]);
            centroids.grow(centers[order_[i]]);
        }
        nodes_[index].min = bounds.min;
        nodes_[index].max = bounds.max;
        if (n <= kLeafSize)
            continue;
        // Binned SAH: the cheapest of kBins - 1 planes per axis across the centroids' bounds.
        int bestAxis = -1, bestSplit = 0;
        float bestCost = bounds.area() * float(n); // as a leaf
        for (int a = 0; a < 3; ++a) {
            const float lo = axis(centroids.min, a), hi = axis(centroids.max, a);
            if (hi - lo < 1e-9f)
                continue;
            Box binBoxes[kBins];
            uint32_t binCounts[kBins] = {};
            const float scale = float(kBins) / (hi - lo);
            for (uint32_t i = first; i < first + n; ++i) {
                const int bin = std::min(kBins - 1, int((axis(centers[order_[i]], a) - lo) * scale));
                binBoxes[bin].grow(boxes[order_[i]]);
                ++binCounts[bin];
            }
            float leftArea[kBins], rightArea[kBins];
            uint32_t leftCount[kBins], rightCount[kBins];
            Box left, right;
            uint32_t l = 0, r = 0;
            for (int i = 0; i < kBins - 1; ++i) {
                left.grow(binBoxes[i]);
                l += binCounts[i];
                leftArea[i] = left.area();
                leftCount[i] = l;
                right.grow(binBoxes[kBins - 1 - i]);
                r += binCounts[kBins - 1 - i];
                rightArea[kBins - 2 - i] = right.area();
                rightCount[kBins - 2 - i] = r;
            }
            for (int i = 0; i < kBins - 1; ++i) {
                const float cost = leftArea[i] * float(leftCount[i]) + rightArea[i] * float(rightCount[i]);
                if (leftCount[i] > 0 && rightCount[i] > 0 && cost < bestCost) {
                    bestCost = cost;
                    bestAxis = a;
                    bestSplit = i;
                }
            }
        }
        uint32_t middle = first;
        if (bestAxis >= 0) {
            const float lo = axis(centroids.min, bestAxis), hi = axis(centroids.max, bestAxis);
            const float scale = float(kBins) / (hi - lo);
            middle = static_cast<uint32_t>(
                std::partition(order_.begin() + first, order_.begin() + first + n,
                               [&](uint32_t t) {
                                   return std::min(kBins - 1, int((axis(centers[t], bestAxis) - lo) * scale)) <= bestSplit;
                               }) -
                order_.begin());
        } else if (n > kLeafSize * 4) { // no cheaper plane (e.g. all centroids equal): halve it anyway, by order
            middle = first + n / 2;
        }
        if (middle == first || middle == first + n)
            continue; // stays a leaf
        const auto child = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back({.first = first, .count = middle - first});
        nodes_.push_back({.first = middle, .count = first + n - middle});
        nodes_[index].first = child;
        nodes_[index].count = 0;
        pending.push_back(child);
        pending.push_back(child + 1);
    }
    prepared_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const BvhTriangle& t = triangles_[order_[i]];
        prepared_[i] = {t.a, bx::sub(t.b, t.a), bx::sub(t.c, t.a)};
    }
}

Aabb Bvh::bounds() const
{
    if (nodes_.empty())
        return {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    return {nodes_[0].min, nodes_[0].max};
}

template <bool AnyHit>
std::optional<BvhHit> Bvh::trace(const bx::Vec3& origin, const bx::Vec3& direction, float tMin, float tMax) const
{
    if (nodes_.empty())
        return std::nullopt;
    const auto inverse = [](float d) { return std::abs(d) > 1e-20f ? 1.0f / d : (d < 0.0f ? -1e20f : 1e20f); };
    const bx::Vec3 inv{inverse(direction.x), inverse(direction.y), inverse(direction.z)};
    std::optional<BvhHit> best;
    float closest = tMax;
    uint32_t stack[kStackSize];
    int top = 0;
    if (hitBox(nodes_[0].min, nodes_[0].max, origin, inv, tMin, closest) == FLT_MAX)
        return std::nullopt;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = nodes_[stack[--top]];
        if (node.count > 0) {
            for (uint32_t i = node.first; i < node.first + node.count; ++i) {
                BvhHit hit;
                const Prepared& p = prepared_[i];
                if (!hitTriangle(p.a, p.ab, p.ac, origin, direction, tMin, closest, hit))
                    continue;
                hit.triangle = order_[i];
                best = hit;
                closest = hit.t;
                if constexpr (AnyHit)
                    return best;
            }
            continue;
        }
        // Both children, the nearer popped first.
        const float near0 = hitBox(nodes_[node.first].min, nodes_[node.first].max, origin, inv, tMin, closest);
        const float near1 = hitBox(nodes_[node.first + 1].min, nodes_[node.first + 1].max, origin, inv, tMin, closest);
        const bool swap = near1 < near0;
        const uint32_t a = swap ? node.first + 1 : node.first, b = swap ? node.first : node.first + 1;
        const float na = swap ? near1 : near0, nb = swap ? near0 : near1;
        if (nb != FLT_MAX && top < kStackSize)
            stack[top++] = b;
        if (na != FLT_MAX && top < kStackSize)
            stack[top++] = a;
    }
    return best;
}

std::optional<BvhHit> Bvh::intersect(const bx::Vec3& origin, const bx::Vec3& direction, float tMin, float tMax) const
{
    return trace<false>(origin, direction, tMin, tMax);
}

bool Bvh::occluded(const bx::Vec3& origin, const bx::Vec3& direction, float tMin, float tMax) const
{
    return trace<true>(origin, direction, tMin, tMax).has_value();
}

std::optional<BvhHit> intersectBruteForce(std::span<const BvhTriangle> triangles, const bx::Vec3& origin,
                                          const bx::Vec3& direction, float tMin, float tMax)
{
    std::optional<BvhHit> best;
    float closest = tMax;
    for (size_t i = 0; i < triangles.size(); ++i) {
        const BvhTriangle& t = triangles[i];
        BvhHit hit;
        if (hitTriangle(t.a, bx::sub(t.b, t.a), bx::sub(t.c, t.a), origin, direction, tMin, closest, hit)) {
            hit.triangle = static_cast<uint32_t>(i);
            best = hit;
            closest = hit.t;
        }
    }
    return best;
}

} // namespace kast
