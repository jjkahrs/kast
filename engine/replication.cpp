#include "replication.h"

#include "components.h"
#include "log.h"
#include "math3d.h"

#include <cmath>
#include <format>

namespace kast {
namespace {

using Json = nlohmann::json;

constexpr float kEpsilon = 1e-4f; // meters / radians: smaller changes (physics jitter) are not a move

bx::Vec3 vec3(const Json& json) { return {json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f)}; }

// Copies source's subtree from one scene into another, under parent. Returns the copy.
entt::entity copyInto(const Scene& from, entt::entity source, Scene& to, entt::entity parent)
{
    const entt::entity copy = to.create(from.registry().get<Name>(source).value, parent);
    copyComponents(from.registry(), source, to.registry(), copy);
    for (entt::entity child : from.registry().get<Hierarchy>(source).children)
        copyInto(from, child, to, copy);
    return copy;
}

entt::entity findByName(const Scene& scene, const std::string& name)
{
    entt::entity found = entt::null;
    scene.forEachDepthFirst([&](entt::entity e) {
        if (found == entt::null && scene.registry().get<Name>(e).value == name)
            found = e;
    });
    return found;
}

} // namespace

bx::Vec3 interpolate(const bx::Vec3& from, const bx::Vec3& to, float elapsed, float duration)
{
    if (duration <= 0.0f || elapsed >= duration)
        return to;
    return bx::lerp(from, to, std::max(elapsed, 0.0f) / duration);
}

bx::Quaternion yawRotation(float heading)
{
    return {0.0f, std::sin(heading * 0.5f), 0.0f, std::cos(heading * 0.5f)};
}

float headingOf(const bx::Quaternion& rotation)
{
    float m[16];
    mtxFromQuat(m, rotation);
    const bx::Vec3 forward = transformDir({0.0f, 0.0f, 1.0f}, m);
    return std::atan2(forward.x, forward.z);
}

bool MoveThrottle::shouldSend(const bx::Vec3& position, float heading, double now, float rateHz)
{
    const bool changed = !position_ || bx::length(bx::sub(position, *position_)) > kEpsilon ||
                         std::abs(heading - heading_) > kEpsilon;
    // The tolerance keeps frame-quantized clocks on the rate (6 frames at 60 fps are 1/10 s, give or take a bit).
    if (!changed || (due_ && now < *due_ - 1e-6))
        return false;
    const double interval = 1.0 / rateHz;
    // On schedule (a frame late at most): the next send is one interval after this one was due, so the average rate
    // stays rateHz instead of drifting to the next frame boundary. After a pause: one interval from now.
    due_ = due_ && now - *due_ < interval ? *due_ + interval : now + interval;
    position_ = position;
    heading_ = heading;
    return true;
}

void MoveThrottle::known(const bx::Vec3& position, float heading)
{
    position_ = position;
    heading_ = heading;
}

void Replication::start(Scene& scene, EventBus& bus, WorldClient& world)
{
    stop(true);
    if (!world.enabled())
        return;
    scene_ = &scene;
    bus_ = &bus;
    world_ = &world;
    throttle_.reset(); // a new scene's player is sent once, wherever it is

    auto managers = scene.registry().view<NetworkReplication>();
    if (managers.size() > 1)
        log::warn("Replication: more than one NetworkReplication in the scene; using the first");
    if (!managers.empty()) {
        const std::string name = managers.get<NetworkReplication>(managers.front()).templateEntity;
        const entt::entity source = findByName(scene, name);
        if (source == entt::null) {
            log::warn(std::format("Replication: template entity '{}' not found; remote players are not shown", name));
        } else {
            template_.emplace();
            copyInto(scene, source, *template_, entt::null);
            scene.destroy(source); // Stop restores it from the Play snapshot
        }
    }

    subscriptions_.push_back(bus.subscribe("world.entered", [this](const Json& e) { placeLocal(e); }));
    subscriptions_.push_back(bus.subscribe("world.correction", [this](const Json& e) { placeLocal(e); }));
    subscriptions_.push_back(bus.subscribe("world.spawn", [this](const Json& e) {
        const bx::Vec3 at = vec3(e.value("position", Json::object()));
        spawn(e.value("entityId", 0u), {.name = e.value("name", ""), .from = at, .to = at,
                                        .heading = e.value("heading", 0.0f)});
    }));
    subscriptions_.push_back(bus.subscribe("world.entityMove", [this](const Json& e) {
        const auto it = remotes_.find(e.value("entityId", 0u));
        if (it == remotes_.end())
            return;
        Remote& remote = it->second;
        remote.from = scene_->valid(remote.entity) ? scene_->registry().get<Transform>(remote.entity).position
                                                   : remote.to;
        remote.to = vec3(e.value("position", Json::object()));
        remote.heading = e.value("heading", 0.0f);
        remote.elapsed = 0.0f;
        if (scene_->valid(remote.entity)) // ponytail: heading snaps; slerp it if turning looks choppy
            scene_->registry().get<Transform>(remote.entity).rotation = yawRotation(remote.heading);
    }));
    subscriptions_.push_back(bus.subscribe("world.despawn", [this](const Json& e) { despawn(e.value("entityId", 0u)); }));
    subscriptions_.push_back(bus.subscribe("world.disconnected", [this](const Json&) { despawnAll(); }));

    // Remote players already in view (a scene switch): their clones in the new scene, where they were last shown.
    std::unordered_map<uint32_t, Remote> known;
    known.swap(remotes_);
    for (auto& [id, remote] : known) {
        remote.from = remote.to;
        spawn(id, remote);
    }
}

void Replication::stop(bool remembered)
{
    subscriptions_.clear();
    template_.reset();
    if (!remembered)
        remotes_.clear();
    for (auto& [id, remote] : remotes_)
        remote.entity = entt::null; // the scene goes; start() re-creates them
    throttle_.reset();
    scene_ = nullptr;
    bus_ = nullptr;
    world_ = nullptr;
}

entt::entity Replication::clone(uint32_t entityId) const
{
    const auto it = remotes_.find(entityId);
    return it == remotes_.end() ? entt::null : it->second.entity;
}

entt::entity Replication::localPlayer() const
{
    auto players = scene_->registry().view<NetworkLocalPlayer>();
    return players.empty() ? entt::null : players.front();
}

float Replication::interpolationSeconds() const
{
    auto managers = scene_->registry().view<NetworkReplication>();
    return managers.empty() ? 0.0f : managers.get<NetworkReplication>(managers.front()).interpolationMs / 1000.0f;
}

void Replication::update(float dt)
{
    if (!scene_)
        return;
    time_ += dt;
    if (const entt::entity player = localPlayer(); player != entt::null && world_->state() == WorldState::InWorld) {
        const Transform& t = scene_->registry().get<Transform>(player);
        const float heading = headingOf(t.rotation);
        if (throttle_.shouldSend(t.position, heading, time_,
                                 scene_->registry().get<NetworkLocalPlayer>(player).sendRateHz))
            world_->move(t.position.x, t.position.y, t.position.z, heading);
    }
    const float duration = interpolationSeconds();
    for (auto& [id, remote] : remotes_) {
        if (!scene_->valid(remote.entity))
            continue; // a script destroyed it: it stays gone until the server despawns and respawns the player
        remote.elapsed += dt;
        scene_->registry().get<Transform>(remote.entity).position =
            interpolate(remote.from, remote.to, remote.elapsed, duration);
    }
}

void Replication::placeLocal(const Json& payload)
{
    const entt::entity player = localPlayer();
    if (player == entt::null)
        return;
    Transform& t = scene_->registry().get<Transform>(player);
    t.position = vec3(payload.value("position", Json::object()));
    if (payload.contains("heading")) // world.entered; a correction carries the position only
        t.rotation = yawRotation(payload.value("heading", 0.0f));
    throttle_.known(t.position, headingOf(t.rotation));
}

void Replication::spawn(uint32_t id, Remote remote)
{
    despawn(id); // an id the server reuses without a despawn we saw
    if (!template_) {
        remotes_[id] = std::move(remote); // remembered, so a scene with a template still shows it
        return;
    }
    const entt::entity root = template_->children(entt::null).front();
    remote.entity = copyInto(*template_, root, *scene_, entt::null);
    entt::registry& registry = scene_->registry();
    registry.get<Name>(remote.entity).value = remote.name;
    Transform& t = registry.get<Transform>(remote.entity);
    t.position = interpolate(remote.from, remote.to, remote.elapsed, interpolationSeconds());
    t.rotation = yawRotation(remote.heading);
    registry.emplace_or_replace<NetworkEntity>(remote.entity, static_cast<int>(id));
    bus_->publish("world.entitySpawned",
                  {{"entityId", id}, {"entity", registry.get<PersistentId>(remote.entity).value}, {"name", remote.name}},
                  "kast.replication");
    remotes_[id] = std::move(remote);
}

void Replication::despawn(uint32_t id)
{
    const auto it = remotes_.find(id);
    if (it == remotes_.end())
        return;
    if (scene_ && scene_->valid(it->second.entity))
        scene_->destroy(it->second.entity);
    remotes_.erase(it);
}

void Replication::despawnAll()
{
    for (auto& [id, remote] : remotes_)
        if (scene_->valid(remote.entity))
            scene_->destroy(remote.entity);
    remotes_.clear();
}

} // namespace kast
