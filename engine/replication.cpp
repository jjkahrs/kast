#include "replication.h"

#include "components.h"
#include "log.h"
#include "lua_components.h"
#include "math3d.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace kast {
namespace {

using Json = nlohmann::json;

constexpr float kEpsilon = 1e-4f; // meters / radians: smaller changes (physics jitter) are not a move
constexpr float kVelocityWindow = 0.5f; // s: a clone's velocity is mostly its last ~5 EntityMoves' (at 10 Hz)

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

MoveHistory::Stamp MoveHistory::sent(const bx::Vec3& position)
{
    deltas_.push_back({++seq_, last_ ? bx::sub(position, *last_) : bx::Vec3{0.0f, 0.0f, 0.0f}});
    if (deltas_.size() > kCapacity) // ponytail: a Correction acking older than this loses those deltas (never at 10 Hz)
        deltas_.pop_front();
    last_ = position;
    return {seq_, epoch_};
}

void MoveHistory::placed(const bx::Vec3& position, std::optional<uint32_t> epoch)
{
    deltas_.clear();
    last_ = position;
    if (epoch)
        epoch_ = *epoch;
}

bx::Vec3 MoveHistory::rebase(const bx::Vec3& position, uint32_t ackSeq, uint32_t epoch, const bx::Vec3& current)
{
    std::erase_if(deltas_, [&](const Delta& d) { return d.seq <= ackSeq; });
    bx::Vec3 rebased = last_ ? bx::add(position, bx::sub(current, *last_)) : position; // + the unsent delta
    for (const Delta& d : deltas_)
        rebased = bx::add(rebased, d.delta);
    epoch_ = epoch;
    last_ = rebased;
    return rebased;
}

bool CorrectionBlend::correct(const bx::Vec3& shown, const bx::Vec3& rebased, float threshold, float duration)
{
    const bx::Vec3 error = bx::sub(shown, rebased);
    if (duration <= 0.0f || bx::length(error) >= threshold) {
        reset();
        return true;
    }
    error_ = error;
    elapsed_ = 0.0f;
    duration_ = duration;
    return false;
}

bx::Vec3 CorrectionBlend::step(float dt)
{
    const bx::Vec3 before = offset();
    elapsed_ += dt;
    return bx::sub(offset(), before);
}

bx::Vec3 CorrectionBlend::offset() const
{
    return interpolate(error_, {0.0f, 0.0f, 0.0f}, elapsed_, duration_); // a straight line to none, like a clone
}

void RemoteMotion::place(const bx::Vec3& position, float heading)
{
    *this = {};
    to_ = position;
    time_ = kVelocityWindow; // standing that long: the first steps' velocity ramps up instead of trusting one sample
    headingFrom_ = heading_ = heading;
}

void RemoteMotion::moved(const bx::Vec3& position, float heading, float duration, float maxExtrapolation)
{
    const bx::Vec3 shown = this->position(duration, maxExtrapolation);
    headingFrom_ = this->heading(duration);
    if (moving_) {
        const float fade = std::exp(-elapsed_ / kVelocityWindow);
        distance_ = bx::add(bx::mul(distance_, fade), bx::sub(position, to_));
        time_ = time_ * fade + std::min(elapsed_, duration + maxExtrapolation); // a restart after a stop: not idle time
        if (time_ > 1e-3f)
            velocity_ = bx::mul(distance_, 1.0f / time_);
    }
    to_ = position;
    heading_ = heading;
    elapsed_ = 0.0f;
    moving_ = true;
    error_ = bx::sub(shown, position);
    const float speed = bx::length(velocity_);
    converge_ = speed > kEpsilon ? std::clamp(bx::length(error_) / (0.35f * speed), duration, std::max(duration, 0.5f))
                                 : duration;
}

bx::Vec3 RemoteMotion::position(float duration, float maxExtrapolation) const
{
    bx::Vec3 path = bx::add(to_, bx::mul(velocity_, std::min(elapsed_, maxExtrapolation)));
    if (elapsed_ > 2.0f * maxExtrapolation) // stopped: back onto the server's position
        path = interpolate(path, to_, elapsed_ - 2.0f * maxExtrapolation, duration);
    return bx::add(path, interpolate(error_, {0.0f, 0.0f, 0.0f}, elapsed_, converge_));
}

float RemoteMotion::heading(float duration) const
{
    if (duration <= 0.0f || elapsed_ >= duration)
        return heading_;
    const float turn = std::remainder(heading_ - headingFrom_, 2.0f * 3.14159265f); // the short way, -pi..pi
    return headingFrom_ + turn * elapsed_ / duration;
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
        const NetworkReplication& manager = managers.get<NetworkReplication>(managers.front());
        const std::string npcFolder = manager.npcTemplates; // copies: destroying entities may move the component
        const std::string name = manager.templateEntity;
        const entt::entity source = findByName(scene, name);
        if (source == entt::null) {
            log::warn(std::format("Replication: template entity '{}' not found; remote players are not shown", name));
        } else {
            template_.emplace();
            copyInto(scene, source, *template_, entt::null);
            scene.destroy(source); // Stop restores it from the Play snapshot
        }
        if (!npcFolder.empty())
            takeNpcTemplates(scene, npcFolder);
    }

    subscriptions_.push_back(bus.subscribe("world.entered", [this](const Json& e) {
        setMap(e);
        placeLocal(e);
    }));
    subscriptions_.push_back(bus.subscribe("world.correction", [this](const Json& e) { reconcile(e); }));
    // Moved by the server (another map, or a teleport): everyone around is gone; the new neighbours Spawn next.
    subscriptions_.push_back(bus.subscribe("world.mapChanged", [this](const Json& e) {
        despawnAll();
        setMap(e);
        placeLocal(e);
    }));
    subscriptions_.push_back(bus.subscribe("world.spawn", [this](const Json& e) {
        const bx::Vec3 at = vec3(e.value("position", Json::object()));
        spawn(e.value("entityId", 0u), {.name = e.value("name", ""), .npc = e.value("kind", "") == "npc",
                                        .templateName = e.value("template", "")},
              at, e.value("heading", 0.0f));
    }));
    subscriptions_.push_back(bus.subscribe("world.entityMove", [this](const Json& e) {
        const auto it = remotes_.find(e.value("entityId", 0u));
        if (it == remotes_.end())
            return;
        it->second.motion.moved(vec3(e.value("position", Json::object())), e.value("heading", 0.0f),
                                interpolationSeconds(), extrapolationSeconds());
    }));
    subscriptions_.push_back(bus.subscribe("world.despawn", [this](const Json& e) { despawn(e.value("entityId", 0u)); }));
    subscriptions_.push_back(bus.subscribe("world.disconnected", [this](const Json&) {
        despawnAll();
        history_.reset();
        localId_.reset();
        localComponents_.clear();
        localPose_.reset();
        map_.reset();
    }));
    subscriptions_.push_back(bus.subscribe("world.component", [this](const Json& e) { applyComponent(e, false); }));
    subscriptions_.push_back(bus.subscribe("world.componentRemoved", [this](const Json& e) { applyComponent(e, true); }));
    applyMap(); // a scene switch: the new scene's terrain shows the map the player is on
    placePlayer(); // and its player stands where the old scene's stood (or where the server put it)
    if (const entt::entity player = localPlayer(); player != entt::null) // a scene switch: the new scene's player
        for (const auto& [name, data] : localComponents_)
            applyComponent(player, name, &data);

    // A scene switch in the world: the new scene's scripts hear where the player is and every replicated value again,
    // as if it had just entered (they missed the originals, the old scene's scripts got them).
    if (localId_ && map_ && localPose_) {
        bus.publish("world.entered",
                    {{"entityId", *localId_}, {"map", *map_},
                     {"position", {{"x", localPose_->position.x}, {"y", localPose_->position.y}, {"z", localPose_->position.z}}},
                     {"heading", headingOf(localPose_->rotation)}},
                    "kast.replication");
        publishComponents(*localId_, localComponents_);
    }
    // Remote players already in view (a scene switch): their clones in the new scene, where they were last shown.
    std::unordered_map<uint32_t, Remote> known;
    known.swap(remotes_);
    for (auto& [id, remote] : known) {
        spawn(id, remote, remote.motion.position(interpolationSeconds(), extrapolationSeconds()),
              remote.motion.heading(interpolationSeconds()));
        publishComponents(id, remotes_[id].components);
    }
}

void Replication::publishComponents(uint32_t id, const std::map<std::string, Json>& components)
{
    for (const auto& [name, data] : components)
        bus_->publish("world.component", {{"entityId", id}, {"name", name}, {"data", data}}, "kast.replication");
}

void Replication::stop(bool remembered)
{
    subscriptions_.clear();
    template_.reset();
    npcTemplates_.clear();
    npcTemplateScene_.reset();
    if (!remembered) {
        remotes_.clear();
        localId_.reset();
        localComponents_.clear();
        localPose_.reset();
        map_.reset();
        history_.reset();
    }
    for (auto& [id, remote] : remotes_)
        remote.entity = entt::null; // the scene goes; start() re-creates them
    throttle_.reset();
    blend_.reset();
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

float Replication::extrapolationSeconds() const
{
    auto managers = scene_->registry().view<NetworkReplication>();
    return managers.empty() ? 0.0f : managers.get<NetworkReplication>(managers.front()).maxExtrapolationMs / 1000.0f;
}

void Replication::showRemote(Remote& remote)
{
    Transform& t = scene_->registry().get<Transform>(remote.entity);
    t.position = remote.motion.position(interpolationSeconds(), extrapolationSeconds());
    const float heading = remote.motion.heading(interpolationSeconds());
    if (remote.shownHeading != heading) {
        t.rotation = yawRotation(heading);
        remote.shownHeading = heading;
    }
}

void Replication::update(float dt)
{
    if (!scene_)
        return;
    time_ += dt;
    if (const entt::entity player = localPlayer(); player != entt::null) {
        Transform& t = scene_->registry().get<Transform>(player);
        t.position = bx::add(t.position, blend_.step(dt)); // the blend on top of what this frame's systems did
        if (world_->state() == WorldState::InWorld) {
            const bx::Vec3 position = bx::sub(t.position, blend_.offset()); // true: the blend is not a move
            localPose_ = {position, t.rotation}; // what this frame's systems did, for the next scene's player
            const float heading = headingOf(t.rotation);
            if (throttle_.shouldSend(position, heading, time_,
                                     scene_->registry().get<NetworkLocalPlayer>(player).sendRateHz)) {
                const MoveHistory::Stamp stamp = history_.sent(position);
                world_->move(position.x, position.y, position.z, heading, stamp.seq, stamp.epoch);
            }
        }
    }
    for (auto& [id, remote] : remotes_) {
        remote.motion.step(dt);
        if (scene_->valid(remote.entity)) // else a script destroyed it: gone until the server respawns the player
            showRemote(remote);
    }
}

void Replication::setMap(const Json& payload)
{
    map_ = payload.value("map", 0);
    applyMap();
}

void Replication::applyMap()
{
    if (map_)
        for (auto [entity, terrain] : scene_->registry().view<Terrain>().each())
            terrain.map = *map_;
}

// Not a placement: the player keeps walking from where it would be had the server known its unacked Moves all along.
// The server does not know the rebased position until a Move of the new epoch says so: the throttle takes only the
// Correction's position as known, so a rebase that moved on from it goes out. A small error blends out of the shown
// position (CorrectionBlend); a large one snaps.
void Replication::reconcile(const Json& payload)
{
    const bx::Vec3 position = vec3(payload.value("position", Json::object()));
    Pose pose = localPose_.value_or(Pose{});
    const entt::entity player = localPlayer();
    if (player != entt::null) // where it truly is: the shown position less what is still blending
        pose = {bx::sub(scene_->registry().get<Transform>(player).position, blend_.offset()),
                scene_->registry().get<Transform>(player).rotation};
    pose.position = history_.rebase(position, payload.value("ackSeq", 0u), payload.value("epoch", 0u), pose.position);
    localPose_ = pose;
    if (player == entt::null)
        return;
    Transform& t = scene_->registry().get<Transform>(player);
    const NetworkLocalPlayer& settings = scene_->registry().get<NetworkLocalPlayer>(player);
    if (blend_.correct(t.position, pose.position, settings.smoothingThreshold, settings.smoothingMs / 1000.0f))
        t.position = pose.position;
    throttle_.known(position, headingOf(t.rotation));
}

// Remembered even when the scene has no local player (a character select scene): the next scene's player gets it.
void Replication::placeLocal(const Json& payload)
{
    if (payload.contains("entityId")) { // world.entered: a new character, so none of the old one's components
        localId_ = payload.value("entityId", 0u);
        localComponents_.clear();
    }
    const bx::Vec3 position = vec3(payload.value("position", Json::object()));
    // The world.entered start() replays after a scene switch puts the player where it already is: not a placement,
    // its Moves in flight still count. (seq and epoch are the connection's: reset by world.disconnected only.)
    const bx::Vec3& was = localPose_ ? localPose_->position : position;
    if (payload.contains("epoch")) // world.mapChanged: always a placement, Moves from before it are dropped
        history_.placed(position, payload.value("epoch", 0u));
    else if (!localPose_ || was.x != position.x || was.y != position.y || was.z != position.z)
        history_.placed(position);
    localPose_ = Pose{position, yawRotation(payload.value("heading", 0.0f))};
    placePlayer();
}

void Replication::placePlayer()
{
    blend_.reset(); // a placement (map change, teleport, scene switch) snaps
    const entt::entity player = localPlayer();
    if (player == entt::null || !localPose_)
        return;
    Transform& t = scene_->registry().get<Transform>(player);
    t.position = localPose_->position;
    t.rotation = localPose_->rotation;
    throttle_.known(t.position, headingOf(t.rotation));
}

void Replication::spawn(uint32_t id, Remote remote, const bx::Vec3& position, float heading)
{
    remote.motion.place(position, heading);
    remote.shownHeading.reset(); // a new clone
    despawn(id); // an id the server reuses without a despawn we saw
    const auto [source, root] = cloneSource(remote);
    if (!source) {
        remotes_[id] = std::move(remote); // remembered, so a scene with a template still shows it
        return;
    }
    remote.entity = copyInto(*source, root, *scene_, entt::null);
    entt::registry& registry = scene_->registry();
    registry.get<Name>(remote.entity).value = remote.name;
    showRemote(remote);
    registry.emplace_or_replace<NetworkEntity>(remote.entity, static_cast<int>(id));
    for (const auto& [name, data] : remote.components)
        applyComponent(remote.entity, name, &data);
    bus_->publish("world.entitySpawned",
                  {{"entityId", id}, {"entity", registry.get<PersistentId>(remote.entity).value}, {"name", remote.name},
                   {"kind", remote.npc ? "npc" : "player"}, {"template", remote.templateName}},
                  "kast.replication");
    remotes_[id] = std::move(remote);
}

void Replication::takeNpcTemplates(Scene& scene, const std::string& folderName)
{
    const entt::entity folder = findByName(scene, folderName);
    if (folder == entt::null) {
        log::warn(std::format("Replication: NPC templates entity '{}' not found; NPCs use the template entity",
                              folderName));
        return;
    }
    npcTemplateScene_.emplace();
    for (entt::entity child : scene.children(folder)) {
        const std::string& name = scene.registry().get<Name>(child).value;
        if (!npcTemplates_.contains(name)) // two templates with one name: the first wins
            npcTemplates_[name] = copyInto(scene, child, *npcTemplateScene_, entt::null);
    }
    scene.destroy(folder); // with every template under it; Stop restores them from the Play snapshot
}

std::pair<const Scene*, entt::entity> Replication::cloneSource(const Remote& remote)
{
    if (remote.npc) {
        if (const auto it = npcTemplates_.find(remote.templateName); it != npcTemplates_.end())
            return {&*npcTemplateScene_, it->second};
        if (warned_.insert("npc template " + remote.templateName).second)
            log::warn(std::format("Replication: no NPC template '{}' under npcTemplates; NPCs using it are clones "
                                  "of the template entity",
                                  remote.templateName));
    }
    if (!template_)
        return {nullptr, entt::null};
    return {&*template_, template_->children(entt::null).front()};
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

void Replication::applyComponent(const Json& event, bool removed)
{
    const uint32_t id = event.value("entityId", 0u);
    const std::string name = event.value("name", "");
    const Json data = event.value("data", Json());
    std::map<std::string, Json>* known = nullptr;
    entt::entity entity = entt::null;
    if (localId_ && *localId_ == id) {
        known = &localComponents_;
        entity = localPlayer();
    } else if (const auto it = remotes_.find(id); it != remotes_.end()) {
        known = &it->second.components;
        entity = it->second.entity;
    } else {
        return; // not in view (the server never sends that)
    }
    if (removed)
        known->erase(name);
    else
        (*known)[name] = data;
    if (scene_->valid(entity))
        applyComponent(entity, name, removed ? nullptr : &data);
}

// data null: remove. The server's values are authoritative: whatever was there is replaced.
void Replication::applyComponent(entt::entity entity, const std::string& name, const Json* data)
{
    entt::registry& registry = scene_->registry();
    const ComponentType type = findAnyComponentType(name);
    if (!type.lua()) {
        if (warned_.insert(name).second)
            log::warn(std::format("Replication: the server sent component \"{}\", which has no Lua component definition "
                                  "in this project; ignored",
                                  name));
        return;
    }
    if (!data) {
        if (hasComponent(registry, entity, type))
            removeComponent(registry, entity, type);
        return;
    }
    std::vector<std::string> fixes;
    // ponytail: event payloads are unordered JSON; a dump + parse gives migrateLuaComponent its ordered form.
    const nlohmann::ordered_json value =
        migrateLuaComponent(*type.lua(), nlohmann::ordered_json::parse(data->dump()), fixes);
    for (const std::string& fix : fixes)
        if (warned_.insert(name + ": " + fix).second)
            log::warn(std::format("Replication: {}: {}: {}", entityLabel(registry, entity), name, fix));
    std::string error;
    setComponentJson(registry, entity, type, value, error); // canonical after migrate: never fails
}

void Replication::despawnAll()
{
    for (auto& [id, remote] : remotes_)
        if (scene_->valid(remote.entity))
            scene_->destroy(remote.entity);
    remotes_.clear();
}

} // namespace kast
