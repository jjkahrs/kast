#include "lod.h"

#include <algorithm>
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

} // namespace kast
