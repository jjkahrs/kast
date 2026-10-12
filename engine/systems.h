#pragma once

#include "scene.h"

// C++ systems: plain functions that advance the scene by dt seconds. Only Play mode ticks them.
namespace kast {

using System = void (*)(Scene& scene, float dt);

// Rotates each Rotator entity's local rotation by degreesPerSecond * dt around its normalized local axis.
// A zero-length axis does nothing.
void rotatorSystem(Scene& scene, float dt);

// Runs every system once, in update order (the list in systems.cpp; add new systems there).
void tickSystems(Scene& scene, float dt);

} // namespace kast
