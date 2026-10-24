#pragma once

#include "math3d.h"

#include <span>

// Mesh LOD selection math for the Renderer, GPU-free so self_check can test it. See docs/engine/instancing-and-culling.html.
namespace kast {

// A level is kept until the screen height leaves its range by this fraction of the boundary it crossed, so a model
// sitting at a threshold doesn't flicker between two levels.
constexpr float kLodHysteresis = 0.1f;

// The fraction of the view's height the box covers through viewMtx / projMtx (bx convention): its largest extent over
// the height the view sees at the distance of its center (perspective) or the view's height (orthographic). Over 1
// when it's larger than the screen; distance-based, so turning the camera doesn't change it.
float lodScreenHeight(const Aabb& bounds, const float* viewMtx, const float* projMtx);

// The level for a screen height: the first whose threshold (decreasing per level) it reaches; below the last, the last
// level, or thresholds.size() (not drawn) if cullBelow. previous: the level chosen last frame (-1: none), kept while
// screenHeight is within kLodHysteresis of its range. No thresholds: level 0.
int selectLod(float screenHeight, std::span<const float> thresholds, bool cullBelow, int previous);

} // namespace kast
