#include "physics.h"

#include "log.h"
#include "terrain.h"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kast {
namespace {

// Two object layers: static bodies never test against each other.
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer.GetValue() == kStaticLayer ? "static" : "moving";
    }
#endif
};

bool layersCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) { return a == kMovingLayer || b == kMovingLayer; }

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override
    {
        return layersCollide(layer, static_cast<JPH::ObjectLayer>(broad.GetValue()));
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return layersCollide(a, b); }
};

// Jolt's process-wide setup, once. ponytail: never torn down; the OS reclaims it at exit.
void initJolt()
{
    static const bool done = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = [](const char* format, ...) {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            log::info(std::format("Jolt: {}", buffer));
        };
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)done;
}

JPH::Vec3 toJolt(const bx::Vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat toJolt(const bx::Quaternion& q) { return {q.x, q.y, q.z, q.w}; }
bx::Vec3 toBx(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
bx::Quaternion toBx(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

float maxAxis(const bx::Vec3& v) { return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}); }
bx::Vec3 absScale(const bx::Vec3& v) { return {std::abs(v.x), std::abs(v.y), std::abs(v.z)}; }

bool same(const bx::Vec3& a, const bx::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
// World scale comes out of a matrix decomposition, so a rotating body's is only equal to float noise.
bool sameScale(const bx::Vec3& a, const bx::Vec3& b)
{
    const auto close = [](float x, float y) { return std::abs(x - y) <= 1e-4f * std::max(1.0f, std::abs(y)); };
    return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
}
bool same(const Collider& a, const Collider& b)
{
    return a.shape == b.shape && same(a.size, b.size) && a.radius == b.radius && a.height == b.height &&
           same(a.center, b.center) && a.isTrigger == b.isTrigger && a.friction == b.friction &&
           a.restitution == b.restitution;
}
bool same(const Rigidbody& a, const Rigidbody& b)
{
    return a.motionType == b.motionType && a.mass == b.mass && a.gravityScale == b.gravityScale &&
           a.linearDamping == b.linearDamping && a.angularDamping == b.angularDamping;
}
bool same(const CharacterController& a, const CharacterController& b)
{
    return a.radius == b.radius && a.height == b.height && a.maxSlopeDegrees == b.maxSlopeDegrees &&
           a.stepHeight == b.stepHeight;
}
template <typename T>
bool same(const std::optional<T>& a, const std::optional<T>& b)
{
    return a.has_value() == b.has_value() && (!a || same(*a, *b));
}

constexpr float kPoseEpsilon = 1e-4f; // meters; a world pose further than this from the last synced one is a teleport

bool moved(const bx::Vec3& a, const bx::Vec3& b) { return bx::distanceSq(a, b) > kPoseEpsilon * kPoseEpsilon; }
bool turned(const bx::Quaternion& a, const bx::Quaternion& b)
{
    return std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 1.0f - 1e-6f;
}

// A capsule of total height along Y, centered on the origin; a sphere when it has no cylinder part.
JPH::Ref<JPH::Shape> capsule(float radius, float height)
{
    const float halfCylinder = height * 0.5f - radius;
    if (halfCylinder <= 1e-4f)
        return new JPH::SphereShape(radius);
    return new JPH::CapsuleShape(halfCylinder, radius);
}

JPH::Ref<JPH::Shape> colliderShape(const Collider& c, const bx::Vec3& worldScale)
{
    const bx::Vec3 scale = absScale(worldScale);
    JPH::Ref<JPH::Shape> shape;
    switch (c.shape) {
    case ColliderShape::Box: {
        const JPH::Vec3 half(c.size.x * scale.x * 0.5f, c.size.y * scale.y * 0.5f, c.size.z * scale.z * 0.5f);
        shape = new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, half.ReduceMin()));
        break;
    }
    case ColliderShape::Sphere:
        shape = new JPH::SphereShape(c.radius * maxAxis(scale));
        break;
    case ColliderShape::Capsule:
        shape = capsule(c.radius * maxAxis(scale), c.height * maxAxis(scale));
        break;
    }
    const bx::Vec3 offset = bx::mul(c.center, worldScale);
    if (same(offset, {0.0f, 0.0f, 0.0f}))
        return shape;
    return new JPH::RotatedTranslatedShape(toJolt(offset), JPH::Quat::sIdentity(), shape);
}

// A terrain tile's ground with its (0, 0) sample at the origin: Jolt's HeightFieldShape splits each quad along the
// same diagonal as map::for_each_triangle. 257 samples are padded to whole 8-sample blocks with no-collision (Jolt
// reads whole blocks when sizing samples); the bits per sample keep quantization under kTerrainError.
constexpr uint32_t kTerrainBlock = 8;
constexpr uint32_t kTerrainSamples = (map::kTileSamples + kTerrainBlock - 1) / kTerrainBlock * kTerrainBlock; // 264
constexpr float kTerrainError = 0.02f; // metres
JPH::ShapeSettings::ShapeResult terrainShape(const TerrainTileData& tile)
{
    JPH::HeightFieldShapeSettings settings;
    settings.mSampleCount = kTerrainSamples;
    settings.mBlockSize = kTerrainBlock;
    settings.mHeightSamples.resize(kTerrainSamples * kTerrainSamples, JPH::HeightFieldShapeConstants::cNoCollisionValue);
    for (uint32_t v = 0; v < uint32_t(map::kTileSamples); ++v)
        for (uint32_t u = 0; u < uint32_t(map::kTileSamples); ++u)
            settings.mHeightSamples[v * kTerrainSamples + u] =
                map::sample_height(tile.heights[v * map::kTileSamples + u], *tile.map);
    settings.mBitsPerSample = std::clamp(settings.CalculateBitsPerSampleForError(kTerrainError), 1u, 8u);
    return settings.Create();
}

// The character's capsule with its bottom at the origin.
JPH::Ref<JPH::Shape> characterShape(const CharacterController& c, float scale)
{
    return new JPH::RotatedTranslatedShape(JPH::Vec3(0.0f, c.height * scale * 0.5f, 0.0f), JPH::Quat::sIdentity(),
                                           capsule(c.radius * scale, c.height * scale));
}

void worldPose(const Scene& scene, entt::entity entity, bx::Vec3& position, bx::Quaternion& rotation, bx::Vec3& scale)
{
    float world[16];
    scene.worldMatrix(entity, world);
    decomposeTrs(world, position, rotation, scale);
}

std::string describe(const Scene& scene, entt::entity entity)
{
    return std::format("'{}' (id {})", scene.registry().get<Name>(entity).value,
                       scene.registry().get<PersistentId>(entity).value);
}

nlohmann::json jsonVec(const bx::Vec3& v) { return {{"x", v.x}, {"y", v.y}, {"z", v.z}}; }

uint32_t key(const JPH::BodyID& id) { return id.GetIndexAndSequenceNumber(); }

} // namespace

const char* physicsConflict(const entt::registry& registry, entt::entity entity)
{
    if (registry.all_of<CharacterController>(entity) && registry.any_of<Collider, Rigidbody>(entity))
        return "a CharacterController can't be combined with a Collider or Rigidbody: no physics is created for it";
    return nullptr;
}

// A contact begin / end reported by Jolt (any thread), handled on the main thread after the step.
struct ContactChange {
    bool added;
    bool character;   // from a CharacterVirtual (else from the body simulation)
    JPH::BodyID a, b; // a character's is its inner body
    bx::Vec3 point{0.0f, 0.0f, 0.0f}, normal{0.0f, 0.0f, 0.0f}; // added only; normal points from a toward b
};

struct Physics::World {
    // One simulated entity.
    struct Body {
        EntityId id = 0;
        JPH::BodyID body; // a character's inner body
        JPH::Ref<JPH::CharacterVirtual> character;
        bool trigger = false;
        // What the body was built from; a difference rebuilds it.
        std::optional<Collider> collider;
        std::optional<Rigidbody> rigidbody;
        std::optional<CharacterController> controller;
        std::shared_ptr<const TerrainTileData> terrain; // a TerrainTile's static ground (no collider)
        bx::Vec3 scale{1.0f, 1.0f, 1.0f};
        // The world pose physics last saw or wrote; a different one is a teleport.
        bx::Vec3 position{0.0f, 0.0f, 0.0f};
        bx::Quaternion rotation{bx::InitIdentity};
        bool movedKinematic = false; // MoveKinematic this step: stop it afterwards
        // Character velocity: horizontal intent (moveCharacter) + vertical fall / jump speed.
        bx::Vec3 walk{0.0f, 0.0f, 0.0f};
        bx::Vec3 fall{0.0f, 0.0f, 0.0f};

        MotionType motion() const { return rigidbody ? rigidbody->motionType : MotionType::Static; }
    };

    // A pair of entities in contact. A character touches a body through two sources (its CharacterVirtual and its
    // inner body); the pair ends when neither touches.
    struct Pair {
        bool viaBodies = false;
        bool viaCharacter = false;
        bool dormant = false; // Jolt ended the body contact because a body fell asleep; recheck when it wakes
        bool trigger = false;
        EntityId a = 0, b = 0;
    };

    class Contacts final : public JPH::ContactListener {
    public:
        explicit Contacts(World& world) : world_(world) {}
        void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override
        {
            const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
            world_.queue({true, false, a.GetID(), b.GetID(), toBx(JPH::Vec3(point)), toBx(manifold.mWorldSpaceNormal)});
        }
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
        {
            world_.queue({false, false, pair.GetBody1ID(), pair.GetBody2ID()});
        }

    private:
        World& world_;
    };

    class CharacterContacts final : public JPH::CharacterContactListener {
    public:
        explicit CharacterContacts(World& world) : world_(world) {}
        void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID&,
                            JPH::RVec3Arg point, JPH::Vec3Arg normal, JPH::CharacterContactSettings&) override
        {
            // Jolt's normal points from the character toward the body (checked in self_check).
            world_.queue({true, true, character->GetInnerBodyID(), body, toBx(JPH::Vec3(point)), toBx(normal)});
        }
        void OnContactRemoved(const JPH::CharacterVirtual* character, const JPH::BodyID& body,
                              const JPH::SubShapeID&) override
        {
            world_.queue({false, true, character->GetInnerBodyID(), body});
        }

    private:
        World& world_;
    };

    World(Scene& scene, EventBus& bus) : scene(scene), bus(bus)
    {
        // ponytail: fixed capacities; raise them if a scene gets near 65536 bodies.
        system.Init(65536, 0, 65536, 10240, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        system.SetContactListener(&contacts);
    }
    ~World()
    {
        bodies.clear(); // characters remove their inner bodies while the system lives
    }

    Scene& scene;
    EventBus& bus;
    BroadPhaseLayers broadPhaseLayers;
    ObjectVsBroadPhase objectVsBroadPhase;
    ObjectPairs objectPairs;
    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                  static_cast<int>(std::max(1u, std::thread::hardware_concurrency()) - 1)};
    JPH::PhysicsSystem system;
    Contacts contacts{*this};
    CharacterContacts characterContacts{*this};
    std::unordered_map<entt::entity, Body> bodies;
    std::unordered_map<uint32_t, entt::entity> byBody; // Jolt body id (with sequence number) -> entity
    std::unordered_set<entt::entity> reported;          // entities whose setup problem was logged
    std::map<std::pair<entt::entity, entt::entity>, Pair> pairs; // lower entity first
    double accumulator = 0.0;
    std::mutex queueMutex;
    std::vector<ContactChange> changes;

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }
    const JPH::BodyInterface& bi() const { return system.GetBodyInterfaceNoLock(); }

    void queue(ContactChange change)
    {
        std::lock_guard lock(queueMutex);
        changes.push_back(change);
    }

    void step();
    void sync();
    void create(entt::entity entity, Body body);
    void destroy(entt::entity entity);
    void syncPose(Body& body, const bx::Vec3& position, const bx::Quaternion& rotation);
    void moveCharacter(Body& body);
    void writeBack();
    void setWorldPose(entt::entity entity, Body& body, const bx::Vec3& position, const bx::Quaternion& rotation);
    void publishContacts();
    void publishExit(const Pair& pair);

    const Body& body(entt::entity entity, bool (*accepts)(const Body&), const char* needs) const;
    Body& body(entt::entity entity, bool (*accepts)(const Body&), const char* needs)
    {
        return const_cast<Body&>(std::as_const(*this).body(entity, accepts, needs));
    }
    static bool isMoving(const Body& b) { return b.character || b.motion() != MotionType::Static; }
    static bool isRigid(const Body& b) { return !b.character && b.motion() != MotionType::Static; }
    static bool isDynamic(const Body& b) { return !b.character && b.motion() == MotionType::Dynamic; }
    static bool isCharacter(const Body& b) { return b.character != nullptr; }
    static constexpr const char* kMovingBody = "dynamic or kinematic Rigidbody or CharacterController";
    static constexpr const char* kRigidBody = "dynamic or kinematic Rigidbody";
    static constexpr const char* kDynamicBody = "dynamic Rigidbody";
    static constexpr const char* kCharacter = "CharacterController";

    std::vector<entt::entity> overlap(const JPH::Shape& shape, JPH::RMat44Arg transform) const;

    entt::entity entityOf(const JPH::BodyID& id) const
    {
        const auto it = byBody.find(key(id));
        return it == byBody.end() ? entt::null : it->second;
    }
};

void Physics::World::step()
{
    system.SetGravity(toJolt(scene.physics.gravity));
    sync();
    // On terrain, characters climb what the map's navmesh will (nav.maxSlopeDeg); elsewhere their own limit.
    std::optional<float> navSlope;
    for (const auto& [entity, body] : bodies)
        if (body.terrain && body.terrain->map->nav) {
            navSlope = body.terrain->map->nav->maxSlopeDeg;
            break;
        }
    for (auto& [entity, body] : bodies)
        if (body.character) {
            body.character->SetMaxSlopeAngle(JPH::DegreesToRadians(navSlope.value_or(body.controller->maxSlopeDegrees)));
            moveCharacter(body);
        }
    system.Update(kPhysicsStep, 1, &temp, &jobs);
    for (auto& [entity, body] : bodies)
        if (body.movedKinematic) {
            bi().SetLinearAndAngularVelocity(body.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            body.movedKinematic = false;
        }
    writeBack();
    publishContacts();
}

// Bodies to match the scene's components; poses to match the Transforms.
void Physics::World::sync()
{
    const entt::registry& registry = scene.registry();
    std::vector<entt::entity> gone;
    for (const auto& [entity, body] : bodies)
        if (!scene.valid(entity) || physicsConflict(registry, entity) ||
            !registry.any_of<Collider, CharacterController, TerrainTile>(entity))
            gone.push_back(entity);
    for (entt::entity entity : gone)
        destroy(entity);

    // Depth-first, so bodies are created in a stable order (Jolt's results depend on it).
    // ponytail: visits every entity each step; keep a physics entity list if scenes get huge.
    scene.forEachDepthFirst([&](entt::entity entity) {
        if (const auto* tile = registry.try_get<TerrainTile>(entity); tile && tile->data) {
            const auto it = bodies.find(entity);
            if (it != bodies.end() && it->second.terrain == tile->data)
                return; // static at its map position: its Transform is ignored
            if (it != bodies.end())
                destroy(entity);
            Body wanted;
            wanted.terrain = tile->data;
            wanted.position = {float(tile->data->tile.x) * map::kTileSize, 0.0f, float(tile->data->tile.z) * map::kTileSize};
            create(entity, std::move(wanted));
            return;
        }
        const auto* collider = registry.try_get<Collider>(entity);
        const auto* rigidbody = registry.try_get<Rigidbody>(entity);
        const auto* controller = registry.try_get<CharacterController>(entity);
        if (!collider && !controller) {
            if (rigidbody && reported.insert(entity).second)
                log::warn(std::format("Physics: entity {} has a Rigidbody but no Collider; it is ignored",
                                      describe(scene, entity)));
            return;
        }
        if (const char* conflict = physicsConflict(registry, entity)) {
            if (reported.insert(entity).second)
                log::error(std::format("Physics: entity {}: {}", describe(scene, entity), conflict));
            return;
        }
        Body wanted;
        if (collider)
            wanted.collider = *collider;
        if (rigidbody)
            wanted.rigidbody = *rigidbody;
        if (controller)
            wanted.controller = *controller;
        bx::Vec3 position{bx::InitNone}, scale{bx::InitNone};
        bx::Quaternion rotation{bx::InitNone};
        worldPose(scene, entity, position, rotation, scale);
        wanted.scale = scale;

        const auto it = bodies.find(entity);
        if (it != bodies.end() && same(it->second.collider, wanted.collider) &&
            same(it->second.rigidbody, wanted.rigidbody) && same(it->second.controller, wanted.controller) &&
            sameScale(it->second.scale, wanted.scale)) {
            syncPose(it->second, position, rotation);
            return;
        }
        if (it != bodies.end()) {
            // Rebuilt: keep moving as before.
            wanted.walk = it->second.walk;
            wanted.fall = it->second.fall;
            destroy(entity);
        }
        wanted.position = position;
        wanted.rotation = rotation;
        create(entity, std::move(wanted));
    });
}

void Physics::World::create(entt::entity entity, Body body)
{
    body.id = scene.registry().get<PersistentId>(entity).value;
    if (body.controller) {
        const CharacterController& c = *body.controller;
        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        const float scale = maxAxis(body.scale);
        settings->mShape = characterShape(c, scale);
        settings->mInnerBodyShape = settings->mShape; // lets bodies, queries and triggers see the character
        settings->mInnerBodyLayer = kMovingLayer;
        settings->mMaxSlopeAngle = JPH::DegreesToRadians(c.maxSlopeDegrees);
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -c.radius * scale); // the lower hemisphere
        body.character = new JPH::CharacterVirtual(settings, JPH::RVec3(toJolt(body.position)), JPH::Quat::sIdentity(),
                                                   0, &system);
        body.character->SetListener(&characterContacts);
        body.body = body.character->GetInnerBodyID();
    } else if (body.terrain) {
        const JPH::ShapeSettings::ShapeResult shape = terrainShape(*body.terrain);
        if (shape.HasError()) {
            log::error(std::format("Physics: terrain tile {}: {}", describe(scene, entity), shape.GetError().c_str()));
            return;
        }
        const JPH::BodyCreationSettings settings(shape.Get(), JPH::RVec3(toJolt(body.position)), JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static, kStaticLayer);
        body.body = bi().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (body.body.IsInvalid()) {
            log::error(std::format("Physics: too many bodies, terrain tile {} has none", describe(scene, entity)));
            return;
        }
    } else {
        const Collider& c = *body.collider;
        const MotionType motion = body.motion();
        body.trigger = c.isTrigger;
        // A trigger is never static in Jolt: kinematic sensors also see static and kinematic bodies.
        const JPH::EMotionType joltMotion = motion == MotionType::Dynamic ? JPH::EMotionType::Dynamic
                                            : motion == MotionType::Kinematic || c.isTrigger ? JPH::EMotionType::Kinematic
                                                                                             : JPH::EMotionType::Static;
        JPH::BodyCreationSettings settings(colliderShape(c, body.scale), JPH::RVec3(toJolt(body.position)),
                                           toJolt(bx::normalize(body.rotation)), joltMotion,
                                           joltMotion == JPH::EMotionType::Static ? kStaticLayer : kMovingLayer);
        settings.mFriction = c.friction;
        settings.mRestitution = c.restitution;
        settings.mIsSensor = c.isTrigger;
        settings.mCollideKinematicVsNonDynamic = c.isTrigger;
        settings.mAllowSleeping = !c.isTrigger; // an asleep sensor misses bodies entering it
        if (body.rigidbody) {
            settings.mGravityFactor = body.rigidbody->gravityScale;
            settings.mLinearDamping = body.rigidbody->linearDamping;
            settings.mAngularDamping = body.rigidbody->angularDamping;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = body.rigidbody->mass;
        }
        body.body = bi().CreateAndAddBody(settings, joltMotion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate
                                                                                           : JPH::EActivation::Activate);
        if (body.body.IsInvalid()) {
            log::error(std::format("Physics: too many bodies, entity {} has none", describe(scene, entity)));
            return;
        }
    }
    byBody[key(body.body)] = entity;
    bodies.emplace(entity, std::move(body));
}

// Removes the entity's body; contacts it had end (exit events) now.
void Physics::World::destroy(entt::entity entity)
{
    const auto it = bodies.find(entity);
    if (it == bodies.end())
        return;
    for (auto pair = pairs.begin(); pair != pairs.end();) {
        if (pair->first.first == entity || pair->first.second == entity) {
            publishExit(pair->second);
            pair = pairs.erase(pair);
        } else {
            ++pair;
        }
    }
    byBody.erase(key(it->second.body));
    if (!it->second.character) {
        bi().RemoveBody(it->second.body);
        bi().DestroyBody(it->second.body);
    }
    bodies.erase(it); // a character's destructor removes its inner body
}

void Physics::World::syncPose(Body& body, const bx::Vec3& position, const bx::Quaternion& rotation)
{
    if (body.character) {
        if (moved(position, body.position))
            body.character->SetPosition(JPH::RVec3(toJolt(position)));
    } else if (moved(position, body.position) || turned(rotation, body.rotation)) {
        const JPH::RVec3 p(toJolt(position));
        const JPH::Quat q = toJolt(bx::normalize(rotation));
        switch (body.motion()) {
        case MotionType::Static:
            bi().SetPositionAndRotation(body.body, p, q, body.trigger ? JPH::EActivation::Activate
                                                                      : JPH::EActivation::DontActivate);
            break;
        case MotionType::Dynamic:
            bi().SetPositionAndRotation(body.body, p, q, JPH::EActivation::Activate);
            break;
        case MotionType::Kinematic:
            bi().MoveKinematic(body.body, p, q, kPhysicsStep);
            body.movedKinematic = true;
            break;
        }
    }
    body.position = position;
    body.rotation = rotation;
}

// Jolt's CharacterVirtual recipe: keep the vertical speed while airborne or rising, drop it on the ground, add
// gravity, walk up steps and stick to the floor going down.
void Physics::World::moveCharacter(Body& body)
{
    JPH::CharacterVirtual& character = *body.character;
    const JPH::Vec3 gravity = system.GetGravity();
    if (character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && body.fall.y <= 0.0f)
        body.fall = {0.0f, 0.0f, 0.0f};
    body.fall = bx::add(body.fall, bx::mul(toBx(gravity), kPhysicsStep));
    character.SetLinearVelocity(toJolt(bx::add(body.walk, body.fall)));

    const float step = body.controller->stepHeight * maxAxis(body.scale);
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mWalkStairsStepUp = JPH::Vec3(0.0f, step, 0.0f);
    settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -step, 0.0f);
    const JPH::IgnoreSingleBodyFilter notSelf(body.body);
    character.ExtendedUpdate(kPhysicsStep, gravity, settings, system.GetDefaultBroadPhaseLayerFilter(kMovingLayer),
                             system.GetDefaultLayerFilter(kMovingLayer), notSelf, {}, temp);
    // ponytail: characters are Y-up; a sideways gravity's horizontal part is dropped here.
    body.fall = {0.0f, character.GetLinearVelocity().GetY(), 0.0f}; // landing, a ceiling
}

void Physics::World::writeBack()
{
    for (auto& [entity, body] : bodies) {
        if (body.character) {
            setWorldPose(entity, body, toBx(JPH::Vec3(body.character->GetPosition())), body.rotation);
        } else if (body.motion() != MotionType::Static && bi().IsActive(body.body)) {
            JPH::RVec3 position;
            JPH::Quat rotation;
            bi().GetPositionAndRotation(body.body, position, rotation);
            setWorldPose(entity, body, toBx(JPH::Vec3(position)), toBx(rotation));
        }
    }
}

// Writes a world pose into the entity's local Transform (its scale stays).
void Physics::World::setWorldPose(entt::entity entity, Body& body, const bx::Vec3& position,
                                  const bx::Quaternion& rotation)
{
    body.position = position;
    body.rotation = rotation;
    Transform& transform = scene.registry().get<Transform>(entity);
    const entt::entity parent = scene.parent(entity);
    if (parent == entt::null) {
        transform.position = position;
        transform.rotation = rotation;
        return;
    }
    float world[16], parentWorld[16], parentInverse[16], local[16];
    mtxFromTrs(world, position, rotation, body.scale);
    scene.worldMatrix(parent, parentWorld);
    bx::mtxInverse(parentInverse, parentWorld);
    bx::mtxMul(local, world, parentInverse); // world = local * parentWorld
    bx::Vec3 scale{bx::InitNone};
    decomposeTrs(local, transform.position, transform.rotation, scale);
}

void Physics::World::publishContacts()
{
    std::vector<ContactChange> batch;
    {
        std::lock_guard lock(queueMutex);
        batch.swap(changes);
    }
    // Jolt also ends the contacts of a body that falls asleep (and starts them again if it wakes still touching).
    const auto asleep = [this](const JPH::BodyID& id) {
        return bi().GetMotionType(id) != JPH::EMotionType::Static && !bi().IsActive(id);
    };
    for (const ContactChange& change : batch) {
        const entt::entity a = entityOf(change.a), b = entityOf(change.b);
        if (a == entt::null || b == entt::null || a == b)
            continue; // a body removed since (its exits went out then)
        const auto pairKey = std::minmax(a, b);
        if (change.added) {
            Pair& pair = pairs[pairKey];
            const bool touching = pair.viaBodies || pair.viaCharacter;
            (change.character ? pair.viaCharacter : pair.viaBodies) = true;
            if (!change.character)
                pair.dormant = false;
            if (touching)
                continue;
            const Body& bodyA = bodies.at(a);
            const Body& bodyB = bodies.at(b);
            pair.trigger = bodyA.trigger || bodyB.trigger;
            pair.a = bodyA.id;
            pair.b = bodyB.id;
            if (pair.trigger)
                bus.publish("triggerEnter", {{"a", pair.a}, {"b", pair.b}});
            else
                bus.publish("collisionEnter", {{"a", pair.a}, {"b", pair.b}, {"point", jsonVec(change.point)},
                                               {"normal", jsonVec(change.normal)}});
        } else {
            const auto it = pairs.find(pairKey);
            if (it == pairs.end())
                continue;
            if (!change.character && (asleep(change.a) || asleep(change.b))) {
                it->second.dormant = true; // still touching, asleep
                continue;
            }
            (change.character ? it->second.viaCharacter : it->second.viaBodies) = false;
            if (it->second.viaBodies || it->second.viaCharacter)
                continue;
            publishExit(it->second);
            pairs.erase(it);
        }
    }
    // A dormant pair whose bodies are awake again without Jolt finding the contact (e.g. one was teleported away) ended.
    for (auto it = pairs.begin(); it != pairs.end();) {
        Pair& pair = it->second;
        if (pair.dormant && !asleep(bodies.at(it->first.first).body) && !asleep(bodies.at(it->first.second).body)) {
            pair.dormant = false;
            pair.viaBodies = false;
            if (!pair.viaCharacter) {
                publishExit(pair);
                it = pairs.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void Physics::World::publishExit(const Pair& pair)
{
    bus.publish(pair.trigger ? "triggerExit" : "collisionExit", {{"a", pair.a}, {"b", pair.b}});
}

const Physics::World::Body& Physics::World::body(entt::entity entity, bool (*accepts)(const Body&),
                                                 const char* needs) const
{
    const auto it = bodies.find(entity);
    if (it == bodies.end() || !accepts(it->second))
        throw std::invalid_argument(std::format(
            "entity {} has no {}", scene.valid(entity) ? scene.registry().get<PersistentId>(entity).value : 0, needs));
    return it->second;
}

Physics::Physics() = default;
Physics::~Physics() = default;

void Physics::start(Scene& scene, EventBus& bus)
{
    stop();
    initJolt();
    world_ = std::make_unique<World>(scene, bus);
    world_->sync(); // bodies exist for queries before the first step
}

void Physics::stop() { world_.reset(); }

int Physics::update(float dt)
{
    if (!world_ || !(dt > 0.0f))
        return 0;
    World& w = *world_;
    w.accumulator += dt;
    int steps = 0;
    // The epsilon keeps 2 x 1/60 from becoming 1 step to float rounding.
    while (w.accumulator >= kPhysicsStep - 1e-6 && steps < kMaxPhysicsSteps) {
        w.step();
        w.accumulator -= kPhysicsStep;
        ++steps;
    }
    if (steps == kMaxPhysicsSteps)
        w.accumulator = std::max(0.0, std::min(w.accumulator, double(kPhysicsStep))); // drop the backlog
    else
        w.accumulator = std::max(0.0, w.accumulator);
    return steps;
}

std::optional<Physics::RayHit> Physics::raycast(const bx::Vec3& origin, const bx::Vec3& direction,
                                                float maxDistance) const
{
    if (!world_)
        return std::nullopt;
    if (!(maxDistance > 0.0f))
        throw std::invalid_argument("maxDistance must be > 0");
    if (bx::length(direction) < 1e-6f)
        return std::nullopt;
    const World& w = *world_;
    const bx::Vec3 dir = bx::normalize(direction);
    const JPH::RRayCast ray{JPH::RVec3(toJolt(origin)), toJolt(bx::mul(dir, maxDistance))};
    JPH::RayCastResult hit;
    if (!w.system.GetNarrowPhaseQueryNoLock().CastRay(ray, hit))
        return std::nullopt;
    const entt::entity entity = w.entityOf(hit.mBodyID);
    if (entity == entt::null)
        return std::nullopt;
    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    JPH::Vec3 normal = JPH::Vec3::sAxisY();
    {
        JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), hit.mBodyID);
        if (lock.Succeeded())
            normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
    }
    return RayHit{entity, toBx(JPH::Vec3(point)), toBx(normal), hit.mFraction * maxDistance};
}

std::vector<entt::entity> Physics::World::overlap(const JPH::Shape& shape, JPH::RMat44Arg transform) const
{
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    system.GetNarrowPhaseQueryNoLock().CollideShape(&shape, JPH::Vec3::sOne(), transform, JPH::CollideShapeSettings(),
                                                      JPH::RVec3::sZero(), collector);
    collector.Sort(); // deepest first, so the order doesn't depend on threads
    std::vector<entt::entity> out;
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const entt::entity entity = entityOf(hit.mBodyID2);
        if (entity != entt::null && std::ranges::find(out, entity) == out.end())
            out.push_back(entity);
    }
    return out;
}

std::vector<entt::entity> Physics::overlapSphere(const bx::Vec3& center, float radius) const
{
    if (!(radius > 0.0f))
        throw std::invalid_argument("radius must be > 0");
    if (!world_)
        return {};
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    return world_->overlap(sphere, JPH::RMat44::sTranslation(JPH::RVec3(toJolt(center))));
}

std::vector<entt::entity> Physics::overlapBox(const bx::Vec3& center, const bx::Vec3& halfExtents,
                                              const bx::Quaternion& rotation) const
{
    if (!(halfExtents.x > 0.0f && halfExtents.y > 0.0f && halfExtents.z > 0.0f))
        throw std::invalid_argument("halfExtents must be > 0 on every axis");
    if (!world_)
        return {};
    const JPH::Vec3 half = toJolt(halfExtents);
    JPH::BoxShape box(half, std::min(JPH::cDefaultConvexRadius, half.ReduceMin()));
    box.SetEmbedded();
    return world_->overlap(box,
                   JPH::RMat44::sRotationTranslation(toJolt(bx::normalize(rotation)), JPH::RVec3(toJolt(center))));
}

bx::Vec3 Physics::velocity(entt::entity entity) const
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    const World::Body& b = world_->body(entity, World::isMoving, World::kMovingBody);
    return b.character ? toBx(b.character->GetLinearVelocity()) : toBx(world_->bi().GetLinearVelocity(b.body));
}

void Physics::setVelocity(entt::entity entity, const bx::Vec3& velocity)
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    World::Body& b = world_->body(entity, World::isMoving, World::kMovingBody);
    if (b.character) {
        b.walk = {velocity.x, 0.0f, velocity.z};
        b.fall = {0.0f, velocity.y, 0.0f};
        b.character->SetLinearVelocity(toJolt(velocity));
    } else {
        world_->bi().SetLinearVelocity(b.body, toJolt(velocity));
    }
}

bx::Vec3 Physics::angularVelocity(entt::entity entity) const
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    return toBx(world_->bi().GetAngularVelocity(world_->body(entity, World::isRigid, World::kRigidBody).body));
}

void Physics::setAngularVelocity(entt::entity entity, const bx::Vec3& velocity)
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    world_->bi().SetAngularVelocity(world_->body(entity, World::isRigid, World::kRigidBody).body, toJolt(velocity));
}

void Physics::addForce(entt::entity entity, const bx::Vec3& force)
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    world_->bi().AddForce(world_->body(entity, World::isDynamic, World::kDynamicBody).body, toJolt(force));
}

void Physics::addImpulse(entt::entity entity, const bx::Vec3& impulse)
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    world_->bi().AddImpulse(world_->body(entity, World::isDynamic, World::kDynamicBody).body, toJolt(impulse));
}

void Physics::moveCharacter(entt::entity entity, const bx::Vec3& velocity)
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    world_->body(entity, World::isCharacter, World::kCharacter).walk = {velocity.x, 0.0f, velocity.z};
}

bool Physics::isGrounded(entt::entity entity) const
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    return world_->body(entity, World::isCharacter, World::kCharacter).character->GetGroundState() ==
           JPH::CharacterVirtual::EGroundState::OnGround;
}

std::optional<Physics::Ground> Physics::ground(entt::entity entity) const
{
    if (!world_)
        throw std::invalid_argument("physics is not running");
    const JPH::CharacterVirtual& character = *world_->body(entity, World::isCharacter, World::kCharacter).character;
    if (!character.IsSupported())
        return std::nullopt;
    const entt::entity ground = world_->entityOf(character.GetGroundBodyID());
    if (ground == entt::null)
        return std::nullopt;
    return Ground{ground, toBx(character.GetGroundNormal())};
}

// --- editor wireframe ---

namespace {

constexpr int kCircleSegments = 24;

// Segments of the circle (or arc from..to radians) around center in the plane of unit axes u, v.
void arc(std::vector<std::pair<bx::Vec3, bx::Vec3>>& out, const bx::Vec3& center, const bx::Vec3& u, const bx::Vec3& v,
         float radius, float from, float to)
{
    const int segments = std::max(2, static_cast<int>(kCircleSegments * (to - from) / (2.0f * std::numbers::pi_v<float>)));
    const auto at = [&](int i) {
        const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(segments);
        return bx::add(center, bx::add(bx::mul(u, radius * std::cos(a)), bx::mul(v, radius * std::sin(a))));
    };
    for (int i = 0; i < segments; ++i)
        out.emplace_back(at(i), at(i + 1));
}

constexpr float kPi = std::numbers::pi_v<float>;
const bx::Vec3 kX{1.0f, 0.0f, 0.0f}, kY{0.0f, 1.0f, 0.0f}, kZ{0.0f, 0.0f, 1.0f};

void capsuleLines(std::vector<std::pair<bx::Vec3, bx::Vec3>>& out, const bx::Vec3& center, float radius, float height)
{
    const float half = std::max(0.0f, height * 0.5f - radius);
    const bx::Vec3 top = bx::add(center, {0.0f, half, 0.0f}), bottom = bx::sub(center, {0.0f, half, 0.0f});
    arc(out, top, kX, kZ, radius, 0.0f, 2.0f * kPi);
    arc(out, bottom, kX, kZ, radius, 0.0f, 2.0f * kPi);
    arc(out, top, kX, kY, radius, 0.0f, kPi);
    arc(out, top, kZ, kY, radius, 0.0f, kPi);
    arc(out, bottom, kX, kY, radius, kPi, 2.0f * kPi);
    arc(out, bottom, kZ, kY, radius, kPi, 2.0f * kPi);
    for (const bx::Vec3& side : {kX, kZ, bx::neg(kX), bx::neg(kZ)})
        out.emplace_back(bx::add(top, bx::mul(side, radius)), bx::add(bottom, bx::mul(side, radius)));
}

} // namespace

std::vector<std::pair<bx::Vec3, bx::Vec3>> colliderWireframe(const Scene& scene, entt::entity entity)
{
    std::vector<std::pair<bx::Vec3, bx::Vec3>> lines; // in the body's unscaled space, then moved to world
    const auto* collider = scene.registry().try_get<Collider>(entity);
    const auto* controller = scene.registry().try_get<CharacterController>(entity);
    if (!collider && !controller)
        return lines;
    bx::Vec3 position{bx::InitNone}, scale{bx::InitNone};
    bx::Quaternion rotation{bx::InitNone};
    worldPose(scene, entity, position, rotation, scale);
    const bx::Vec3 size = absScale(scale);
    const float s = maxAxis(scale);

    if (controller) {
        capsuleLines(lines, {0.0f, controller->height * s * 0.5f, 0.0f}, controller->radius * s, controller->height * s);
        rotation = bx::Quaternion{bx::InitIdentity}; // characters stay upright
    } else {
        const bx::Vec3 center = bx::mul(collider->center, scale);
        switch (collider->shape) {
        case ColliderShape::Box: {
            const bx::Vec3 h = bx::mul(bx::mul(collider->size, size), 0.5f);
            const auto corner = [&](int i) {
                return bx::add(center, {i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z});
            };
            for (int i = 0; i < 8; ++i)
                for (int bit : {1, 2, 4})
                    if (!(i & bit))
                        lines.emplace_back(corner(i), corner(i | bit));
            break;
        }
        case ColliderShape::Sphere:
            arc(lines, center, kX, kY, collider->radius * s, 0.0f, 2.0f * kPi);
            arc(lines, center, kY, kZ, collider->radius * s, 0.0f, 2.0f * kPi);
            arc(lines, center, kX, kZ, collider->radius * s, 0.0f, 2.0f * kPi);
            break;
        case ColliderShape::Capsule:
            capsuleLines(lines, center, collider->radius * s, collider->height * s);
            break;
        }
    }
    float pose[16];
    mtxFromTrs(pose, position, rotation, {1.0f, 1.0f, 1.0f});
    for (auto& [a, b] : lines) {
        a = bx::mul(a, pose);
        b = bx::mul(b, pose);
    }
    return lines;
}

} // namespace kast
