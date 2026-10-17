#pragma once

#include "event_bus.h"
#include "scene.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Physics: a Jolt world built from a scene's Collider / Rigidbody / CharacterController entities (and TerrainTiles as
// static height fields) for one Play session
// (owned by LuaSystems, used from Lua as kast.physics). Never runs in edit mode. See docs/engine/physics.html.
namespace kast {

constexpr float kPhysicsStep = 1.0f / 60.0f; // fixed simulation step, seconds
constexpr int kMaxPhysicsSteps = 4;          // per update(); time past that is dropped (the game slows down)

// Why an entity's physics can't be created, or nullptr. Shared by the simulation and the Inspector.
const char* physicsConflict(const entt::registry& registry, entt::entity entity);

// World-space line segments outlining the entity's Collider or CharacterController (empty if it has neither), for
// the editor's selection wireframe.
std::vector<std::pair<bx::Vec3, bx::Vec3>> colliderWireframe(const Scene& scene, entt::entity entity);

class Physics {
public:
    struct RayHit {
        entt::entity entity;
        bx::Vec3 point;
        bx::Vec3 normal;
        float distance;
    };
    struct Ground {
        entt::entity entity;
        bx::Vec3 normal;
    };

    Physics();
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    // Builds the world from scene and publishes contact events on bus. Stops any running session first. scene and
    // bus must outlive the session.
    void start(Scene& scene, EventBus& bus);
    // Drops the world (no exit events). The scene keeps the poses physics last wrote.
    void stop();
    bool running() const { return world_ != nullptr; }

    // Advances the simulation by whole kPhysicsStep steps for dt seconds of game time (carrying the remainder over),
    // at most kMaxPhysicsSteps. Before each step bodies are created / removed / rebuilt to match the scene's
    // components, and an entity whose world pose changed since the last step is teleported (kinematic: moved over
    // the step). After each step dynamic, kinematic and character poses are written back into Transforms and contact
    // events are published. Returns the number of steps taken.
    int update(float dt);

    // Queries (sensors and characters included). direction need not be normalized; a zero direction misses.
    std::optional<RayHit> raycast(const bx::Vec3& origin, const bx::Vec3& direction, float maxDistance) const;
    std::vector<entt::entity> overlapSphere(const bx::Vec3& center, float radius) const;
    std::vector<entt::entity> overlapBox(const bx::Vec3& center, const bx::Vec3& halfExtents,
                                         const bx::Quaternion& rotation) const;

    // Body control. Throw std::invalid_argument when the entity has no body of the right kind:
    // velocities need a dynamic or kinematic body or a character (angular: no character), forces / impulses a
    // dynamic body, the character functions a CharacterController.
    bx::Vec3 velocity(entt::entity entity) const;
    void setVelocity(entt::entity entity, const bx::Vec3& velocity);
    bx::Vec3 angularVelocity(entt::entity entity) const;
    void setAngularVelocity(entt::entity entity, const bx::Vec3& velocity);
    void addForce(entt::entity entity, const bx::Vec3& force);     // over the next step
    void addImpulse(entt::entity entity, const bx::Vec3& impulse); // instant velocity change * mass
    // The character walks at velocity's horizontal part from now on (until the next call); gravity is added.
    void moveCharacter(entt::entity entity, const bx::Vec3& velocity);
    bool isGrounded(entt::entity entity) const;
    std::optional<Ground> ground(entt::entity entity) const;

private:
    struct World;
    std::unique_ptr<World> world_;
};

} // namespace kast
