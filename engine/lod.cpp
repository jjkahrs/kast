#include "lod.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

namespace kast {

float lodScreenHeight(const Aabb& bounds, const float* viewMtx, const float* projMtx)
{
    const bx::Vec3 extent = bx::sub(bounds.max, bounds.min);
    const float size = std::max({extent.x, extent.y, extent.z});
    // projMtx[5] is 1 / tan(fovY / 2) (perspective) or 2 / height (orthographic, projMtx[15] = 1).
    if (projMtx[15] != 0.0f)
        return 0.5f * size * projMtx[5];
    float cameraWorld[16];
    bx::mtxInverse(cameraWorld, viewMtx);
    const bx::Vec3 eye{cameraWorld[12], cameraWorld[13], cameraWorld[14]};
    const bx::Vec3 center = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    const float distance = std::max(bx::length(bx::sub(center, eye)), 1e-4f);
    return 0.5f * size * projMtx[5] / distance;
}

int selectLod(float screenHeight, std::span<const float> thresholds, bool cullBelow, int previous)
{
    const int count = static_cast<int>(thresholds.size());
    if (count == 0)
        return 0;
    int level = 0;
    while (level < count && screenHeight < thresholds[static_cast<size_t>(level)])
        ++level;
    if (level == count && !cullBelow)
        level = count - 1;
    if (previous < 0 || previous > (cullBelow ? count : count - 1) || previous == level)
        return level;
    // previous's range [low, high): from its threshold (0 for the last level without cullBelow, and for "not drawn")
    // up to the one before it (none for LOD0).
    const auto at = [&](int i) { return thresholds[static_cast<size_t>(i)]; };
    const float low = previous == count || (previous == count - 1 && !cullBelow) ? 0.0f : at(previous);
    const float high = previous == 0 ? FLT_MAX : at(previous - 1);
    if (screenHeight >= low * (1.0f - kLodHysteresis) && screenHeight < high * (1.0f + kLodHysteresis))
        return previous;
    return level;
}

std::pair<int, int> terrainChunkCell(int chunk)
{
    int u = 0, v = 0;
    for (int bit = 0; (1 << (2 * bit)) < kTerrainChunks; ++bit) {
        u |= ((chunk >> (2 * bit)) & 1) << bit;
        v |= ((chunk >> (2 * bit + 1)) & 1) << bit;
    }
    return {u, v};
}

uint32_t terrainSkirtVertex(int u, int v)
{
    constexpr uint32_t n = map::kTileSamples, base = n * n;
    if (u % kTerrainChunkQuads == 0)
        return base + uint32_t(u / kTerrainChunkQuads) * n + uint32_t(v);
    return base + uint32_t(kTerrainChunksPerEdge + 1 + v / kTerrainChunkQuads) * n + uint32_t(u);
}

float terrainSkirtDepth(std::span<const float> heights, int u, int v)
{
    constexpr int n = map::kTileSamples, q = kTerrainChunkQuads;
    // The most the chunk edge from (u0, v0) along u (alongU) or v is off the full one at any stride.
    const auto edgeError = [&](int u0, int v0, bool alongU) {
        const auto h = [&](int i) { return heights[alongU ? size_t(v0) * n + size_t(u0 + i) : size_t(v0 + i) * n + size_t(u0)]; };
        float error = 0.0f;
        for (int stride = 2; stride <= 1 << (kTerrainStrides - 1); stride *= 2)
            for (int i = 0; i <= q; ++i) {
                const int a = i / stride * stride, b = std::min(a + stride, q);
                const float t = float(i - a) / float(stride);
                error = std::max(error, std::abs(h(i) - (h(a) + t * (h(b) - h(a)))));
            }
        return error;
    };
    float error = 0.0f;
    if (u % q == 0) // on a u line: its edges through v
        for (int v0 : {(v - 1) / q * q, v / q * q})
            if (v0 >= 0 && v0 + q < n && v0 <= v && v <= v0 + q)
                error = std::max(error, edgeError(u, v0, false));
    if (v % q == 0)
        for (int u0 : {(u - 1) / q * q, u / q * q})
            if (u0 >= 0 && u0 + q < n && u0 <= u && u <= u0 + q)
                error = std::max(error, edgeError(u0, v, true));
    return 2.0f * error + kTerrainSkirtMargin;
}

uint32_t terrainChunkIndexCount(int stride, bool skirts)
{
    const auto quads = uint32_t(kTerrainChunkQuads / stride);
    return quads * quads * 6 + (skirts ? 4 * quads * 6 : 0);
}

void appendTerrainChunk(int chunk, int stride, bool skirts, std::vector<uint32_t>& out)
{
    constexpr uint32_t n = map::kTileSamples;
    const auto [cu, cv] = terrainChunkCell(chunk);
    const int u0 = cu * kTerrainChunkQuads, v0 = cv * kTerrainChunkQuads;
    const int u1 = u0 + kTerrainChunkQuads, v1 = v0 + kTerrainChunkQuads;
    const auto grid = [&](int u, int v) { return uint32_t(v) * n + uint32_t(u); };
    for (int v = v0; v < v1; v += stride)
        for (int u = u0; u < u1; u += stride) {
            const uint32_t i00 = grid(u, v), i10 = grid(u + stride, v), i01 = grid(u, v + stride),
                           i11 = grid(u + stride, v + stride);
            out.insert(out.end(), {i00, i01, i11, i00, i11, i10});
        }
    if (!skirts)
        return;
    // Around the edge with the outside on the left seen from above (+z along u0, +x along v1, -z along u1, -x along
    // v0): (top a, bottom a, top b), (top b, bottom a, bottom b) then face out, as the grid faces up.
    const auto skirt = [&](int ua, int va, int ub, int vb) {
        const uint32_t ta = grid(ua, va), tb = grid(ub, vb), ba = terrainSkirtVertex(ua, va), bb = terrainSkirtVertex(ub, vb);
        out.insert(out.end(), {ta, ba, tb, tb, ba, bb});
    };
    for (int v = v0; v < v1; v += stride)
        skirt(u0, v, u0, v + stride);
    for (int u = u0; u < u1; u += stride)
        skirt(u, v1, u + stride, v1);
    for (int v = v1; v > v0; v -= stride)
        skirt(u1, v, u1, v - stride);
    for (int u = u1; u > u0; u -= stride)
        skirt(u, v0, u - stride, v0);
}

float terrainChunkError(std::span<const float> heights, int chunk, int stride)
{
    constexpr int n = map::kTileSamples;
    const auto [cu, cv] = terrainChunkCell(chunk);
    const int u0 = cu * kTerrainChunkQuads, v0 = cv * kTerrainChunkQuads;
    const auto h = [&](int u, int v) { return heights[size_t(v) * n + size_t(u)]; };
    float error = 0.0f;
    for (int qv = v0; qv < v0 + kTerrainChunkQuads; qv += stride)
        for (int qu = u0; qu < u0 + kTerrainChunkQuads; qu += stride) {
            const float h00 = h(qu, qv), h10 = h(qu + stride, qv), h01 = h(qu, qv + stride),
                        h11 = h(qu + stride, qv + stride);
            for (int v = 0; v <= stride; ++v)
                for (int u = 0; u <= stride; ++u) {
                    const float fu = float(u) / float(stride), fv = float(v) / float(stride);
                    // The quad's two triangles split along its (0,0)-(1,1) diagonal (appendTerrainChunk).
                    const float coarse = fv >= fu ? h00 + fv * (h01 - h00) + fu * (h11 - h01)
                                                  : h00 + fu * (h10 - h00) + fv * (h11 - h10);
                    error = std::max(error, std::abs(h(qu + u, qv + v) - coarse));
                }
        }
    return error;
}

int terrainLevel(const Aabb& chunk, const bx::Vec3& eye, const float* projMtx,
                 std::span<const float, kTerrainStrides> errors, float bias)
{
    const bx::Vec3 nearest = bx::min(bx::max(eye, chunk.min), chunk.max);
    const float distance = bx::length(bx::sub(nearest, eye));
    const bool ortho = projMtx[15] != 0.0f;
    for (int level = kTerrainStrides - 1; level > 0; --level) {
        const float error = errors[size_t(level)];
        // As lodScreenHeight: projMtx[5] is 1 / tan(fovY / 2) or 2 / height (orthographic).
        const float screen = ortho ? 0.5f * error * projMtx[5] : 0.5f * error * projMtx[5] / std::max(distance, 1e-4f);
        if (screen * bias <= kTerrainMaxScreenError && distance >= kTerrainStrideDistance * float(1 << level) * bias)
            return level;
    }
    return 0;
}

} // namespace kast
