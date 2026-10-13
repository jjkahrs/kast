#pragma once

#include "math3d.h"

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace kast {

using EntityId = uint64_t; // persistent across save/load; 0 is never a valid id

struct PersistentId {
    EntityId value = 0;
};

struct Name {
    std::string value;
};

// Local transform, relative to the parent.
struct Transform {
    bx::Vec3 position{0.0f, 0.0f, 0.0f};
    bx::Quaternion rotation{bx::InitIdentity};
    bx::Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Every scene entity has one. Root entities have parent == entt::null and are ordered by Scene::children(null).
struct Hierarchy {
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

enum class Primitive { Cube, Plane, Sphere };

struct MeshRenderer {
    Primitive primitive = Primitive::Cube;
    bx::Vec3 color{0.8f, 0.8f, 0.8f}; // linear RGB 0..1
};

// Shines along the entity's world -Z axis.
struct DirectionalLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

// Renders the scene from the entity's world position looking down its world -Z (scale is ignored).
struct Camera {
    float verticalFovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

// Spins the entity's local rotation around its local axis while playing (see rotatorSystem).
struct Rotator {
    bx::Vec3 axis{0.0f, 1.0f, 0.0f};
    float degreesPerSecond = 45.0f;
};

// Gives the entity its own finite state machine in Play mode (see state_machines.h): machine is a definition name
// (fsms/*.lua), initialState the state it enters at Play start (empty or unknown: the definition's initial).
struct StateMachine {
    std::string machine;
    std::string initialState;
};

// Shows a game GUI over the Game view in Play mode (see scene_gui.h): path is a project-relative .rml file.
struct UIDocument {
    std::string path;
    bool visible = true;
};

// Local-space bounds of each built-in primitive (all fit the unit cube centered on the origin).
Aabb primitiveBounds(Primitive primitive);

// An entity hierarchy on an EnTT registry. Entities are created and destroyed only through Scene so the
// Hierarchy links and root list stay consistent; other components can be added directly via registry().
class Scene {
public:
    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    // New entity with Name, Transform, Hierarchy and a fresh PersistentId, appended to parent's children.
    entt::entity create(const std::string& name, entt::entity parent = entt::null);
    // Same, with an explicit id (used by loading). The caller guarantees the id is unique and nonzero.
    entt::entity createWithId(EntityId id, const std::string& name, entt::entity parent = entt::null);

    bool valid(entt::entity entity) const { return entity != entt::null && registry_.valid(entity); }

    // Destroys the entity and its whole subtree.
    void destroy(entt::entity entity);

    // Copies the subtree with new ids, inserted right after the original among its siblings. Returns the copy.
    entt::entity duplicate(entt::entity entity);

    // Moves entity under newParent (entt::null = root), appended last. Keeps the world transform.
    // Returns false and changes nothing if newParent is entity itself or one of its descendants.
    bool reparent(entt::entity entity, entt::entity newParent);

    // True if ancestor is entity or one of its ancestors.
    bool isSelfOrAncestor(entt::entity ancestor, entt::entity entity) const;

    entt::entity parent(entt::entity entity) const { return registry_.get<Hierarchy>(entity).parent; }
    // Ordered children; entt::null gives the roots.
    const std::vector<entt::entity>& children(entt::entity entity) const;

    void localMatrix(entt::entity entity, float* out) const;
    void worldMatrix(entt::entity entity, float* out) const;

    // Depth-first, parents before children, in child order.
    template <typename Fn>
    void forEachDepthFirst(Fn&& fn) const
    {
        for (entt::entity root : roots_)
            visit(root, fn);
    }

    // Lua system scripts in run order, relative to the project root (see project.h, lua_systems.h).
    std::vector<std::string> systems;

    // Opaque data owned by the editor (e.g. camera pose), saved under "editor" in the scene file.
    nlohmann::ordered_json editorData = nlohmann::ordered_json::object();

private:
    template <typename Fn>
    void visit(entt::entity entity, Fn& fn) const
    {
        fn(entity);
        for (entt::entity child : registry_.get<Hierarchy>(entity).children)
            visit(child, fn);
    }

    std::vector<entt::entity>& siblings(entt::entity parent);
    entt::entity copySubtree(entt::entity source, entt::entity parent);

    entt::registry registry_;
    std::vector<entt::entity> roots_;
    EntityId nextId_ = 1;
};

// The camera the game renders from: the first Camera entity in depth-first order, or entt::null if there is none.
entt::entity activeCamera(const Scene& scene);

// View and projection matrices of a Camera entity for a viewport of the given width / height.
void cameraMatrices(const Scene& scene, entt::entity camera, float aspect, bool homogeneousDepth, float* view, float* proj);

} // namespace kast
