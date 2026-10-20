#include "systems.h"

#include "animation.h"
#include "particles.h"

#include <cmath>

namespace kast {

void rotatorSystem(Scene& scene, float dt)
{
    for (auto [entity, rotator, transform] : scene.registry().view<Rotator, Transform>().each()) {
        const float length = bx::length(rotator.axis);
        if (length < 1e-6f)
            continue;
        const bx::Vec3 axis = bx::mul(rotator.axis, 1.0f / length);
        const float half = bx::toRad(rotator.degreesPerSecond * dt) * 0.5f;
        const float s = std::sin(half);
        const bx::Quaternion delta{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
        // Hamilton product rotation * delta: delta applies first, i.e. about the entity's local axis.
        transform.rotation = bx::normalize(bx::mul(transform.rotation, delta));
    }
}

void tickSystems(Scene& scene, float dt)
{
    static constexpr System kSystems[] = {rotatorSystem, animatorSystem, particleSystem};
    for (System system : kSystems)
        system(scene, dt);
}

} // namespace kast
