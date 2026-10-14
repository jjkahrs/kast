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

// material: a project-relative .mat file (see material.h); empty = the default grey material.
// castShadows / receiveShadows: into / from the shadow maps of shadow-casting lights (see renderer.h).
struct MeshRenderer {
    Primitive primitive = Primitive::Cube;
    std::string material;
    bool castShadows = true;
    bool receiveShadows = true;
};

// model: a project-relative .glb/.gltf/.fbx/.obj file (see model.h). materials: per-slot .mat overrides by slot index;
// empty (or past the end) = the model's embedded material.
// castShadows / receiveShadows: as MeshRenderer.
struct ModelRenderer {
    std::string model;
    std::vector<std::string> materials;
    bool castShadows = true;
    bool receiveShadows = true;
};

constexpr float kMaxAnimatorSpeed = 10.0f;

// Plays the entity's ModelRenderer's skeletal animation clips (see animation.h). Without a ModelRenderer it does
// nothing. clip: a clip name of the model (empty: bind pose).
struct Animator {
    std::string clip;
    bool loop = true;
    float speed = 1.0f; // 0..kMaxAnimatorSpeed
    bool playOnStart = true;
};

constexpr int kMaxShadowCascades = 4;

// Shines along the entity's world -Z axis. castShadows: cascaded shadow maps out to maxDistance meters from the eye,
// in cascadeCount (1..kMaxShadowCascades) slices. bias: meters along the light, normalBias: shadow map texels along
// the surface normal; both push the receiver toward the light against shadow acne.
struct DirectionalLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool castShadows = true;
    float bias = 0.05f;       // >= 0
    float normalBias = 1.5f;  // >= 0
    float maxDistance = 100.0f; // > 0
    int cascadeCount = kMaxShadowCascades;
};

// Lights meshes within range meters of the entity's world position (see pointLightFalloff in renderer.h).
struct PointLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f}; // linear RGB 0..1
    float intensity = 1.0f;
    float range = 10.0f;
    bool castShadows = false; // a cube shadow map; only the kMaxShadowedPointLights nearest the eye get one
    float bias = 0.05f;       // >= 0, meters toward the light against shadow acne
};

// Per-scene light settings. The defaults match the renderer's former fixed 0.2 grey ambient.
struct SceneLighting {
    bx::Vec3 ambientColor{1.0f, 1.0f, 1.0f}; // linear RGB 0..1
    float ambientIntensity = 0.2f;           // >= 0
};

enum class Tonemap { None, Aces }; // None: clamp to 0..1 (the look before post-processing)

// Per-scene post-processing (see post_process.h) and distance fog (fs_mesh.sc). Every effect starts off, so a scene
// without settings renders as it did before. Ranges are what the scene file accepts.
struct ScenePostProcessing {
    struct Tonemapping {
        bool enabled = false;
        float exposure = 0.0f; // EV, -10..10: color * 2^exposure
        Tonemap op = Tonemap::None;
    } tonemapping;
    struct Bloom {
        bool enabled = false;
        float threshold = 1.0f; // >= 0, HDR luminance where the glow starts
        float intensity = 0.5f; // >= 0
        float radius = 0.5f;    // 0..1, how far the glow spreads
    } bloom;
    struct ColorGrading {
        bool enabled = false;
        float contrast = 1.0f;                // 0..2
        float saturation = 1.0f;              // 0..2
        bx::Vec3 colorFilter{1.0f, 1.0f, 1.0f}; // linear RGB 0..1, multiplied in
        float temperature = 0.0f;             // -1 (cool, blue) .. 1 (warm, orange)
    } colorGrading;
    struct Vignette {
        bool enabled = false;
        float intensity = 0.3f;  // 0..1
        float smoothness = 0.5f; // 0.01..1
    } vignette;
    struct Fxaa {
        bool enabled = false;
    } fxaa;
    struct Fog {
        bool enabled = false;
        bx::Vec3 color{0.5f, 0.6f, 0.7f}; // linear RGB 0..1; the view clears to it while enabled
        float density = 0.05f;          // >= 0, per meter
        float start = 0.0f;             // >= 0, meters from the eye before fog begins
    } fog;
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

enum class ColliderShape { Box, Sphere, Capsule };

// A physics shape (see physics.h). Without a Rigidbody the entity is a static body. size: box full extents; radius:
// sphere / capsule; height: capsule total height including both hemispheres (>= 2 * radius), along local Y. center:
// local offset. World scale applies: box per axis, sphere / capsule by the largest axis. isTrigger: reports
// triggerEnter / triggerExit and blocks nothing.
struct Collider {
    ColliderShape shape = ColliderShape::Box;
    bx::Vec3 size{1.0f, 1.0f, 1.0f}; // > 0
    float radius = 0.5f;             // > 0
    float height = 2.0f;             // > 0, >= 2 * radius
    bx::Vec3 center{0.0f, 0.0f, 0.0f};
    bool isTrigger = false;
    float friction = 0.5f;    // >= 0
    float restitution = 0.0f; // 0..1
};

enum class MotionType { Static, Dynamic, Kinematic };

// Makes the entity's Collider move: Dynamic is simulated, Kinematic follows its Transform (and velocity), Static never
// moves. Needs a Collider (without one no body is created).
struct Rigidbody {
    MotionType motionType = MotionType::Dynamic;
    float mass = 1.0f;           // > 0 kg
    float gravityScale = 1.0f;   // multiplies the scene gravity
    float linearDamping = 0.05f; // >= 0
    float angularDamping = 0.05f; // >= 0
};

// An upright capsule moved by kast.physics.moveCharacter (Jolt CharacterVirtual). The entity's position is the
// capsule's bottom (feet); physics never changes its rotation. Not combinable with Collider / Rigidbody.
struct CharacterController {
    float radius = 0.4f;           // > 0
    float height = 1.8f;           // total, >= 2 * radius
    float maxSlopeDegrees = 45.0f; // 0..90, steeper slopes block
    float stepHeight = 0.3f;       // >= 0, steps up to this tall are climbed
};

// Per-scene physics settings.
struct ScenePhysics {
    bx::Vec3 gravity{0.0f, -9.81f, 0.0f}; // m/s^2
};

// Local-space bounds of each built-in primitive (all fit the unit cube centered on the origin).
Aabb primitiveBounds(Primitive primitive);

// Deepest parent chain a scene may have (a root is level 1): the hierarchy walks recurse, so loading, create and
// reparent refuse anything deeper.
constexpr int kMaxHierarchyDepth = 1000;

// An entity hierarchy on an EnTT registry. Entities are created and destroyed only through Scene so the
// Hierarchy links and root list stay consistent; other components can be added directly via registry().
class Scene {
public:
    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    // New entity with Name, Transform, Hierarchy and a fresh PersistentId, appended to parent's children.
    // Returns entt::null and creates nothing if parent is already kMaxHierarchyDepth levels deep.
    entt::entity create(const std::string& name, entt::entity parent = entt::null);
    // Same, with an explicit id (used by loading). The caller guarantees the id is unique and nonzero.
    entt::entity createWithId(EntityId id, const std::string& name, entt::entity parent = entt::null);

    bool valid(entt::entity entity) const { return entity != entt::null && registry_.valid(entity); }

    // Destroys the entity and its whole subtree. No-op if entity isn't valid.
    void destroy(entt::entity entity);

    // Copies the subtree with new ids, inserted right after the original among its siblings. Returns the copy.
    entt::entity duplicate(entt::entity entity);

    // Moves entity under newParent (entt::null = root), appended last. Keeps the world transform.
    // Returns false and changes nothing if either isn't valid, newParent is entity itself or one of its descendants,
    // or the move would put part of entity's subtree deeper than kMaxHierarchyDepth.
    bool reparent(entt::entity entity, entt::entity newParent);

    // True if ancestor is entity or one of its ancestors.
    bool isSelfOrAncestor(entt::entity ancestor, entt::entity entity) const;

    entt::entity parent(entt::entity entity) const { return registry_.get<Hierarchy>(entity).parent; }
    // Levels from the top: 1 for a root.
    int depth(entt::entity entity) const;
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

    // Saved under "lighting" in the scene file.
    SceneLighting lighting;
    // Saved under "postProcessing" in the scene file.
    ScenePostProcessing postProcessing;
    // Saved under "physics" in the scene file.
    ScenePhysics physics;

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
    int height(entt::entity entity) const; // levels in entity's subtree: 1 for a leaf

    entt::registry registry_;
    std::vector<entt::entity> roots_;
    EntityId nextId_ = 1;
};

// The camera the game renders from: the first Camera entity in depth-first order, or entt::null if there is none.
entt::entity activeCamera(const Scene& scene);

// View and projection matrices of a Camera entity for a viewport of the given width / height.
void cameraMatrices(const Scene& scene, entt::entity camera, float aspect, bool homogeneousDepth, float* view, float* proj);

} // namespace kast
