#pragma once

#include "event_bus.h"
#include "scene.h"
#include "world_client.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Replication: networked players without sync code (docs/engine/online-services.html#replication). Consumes the world
// client's "world.*" events from the event bus and drives the scene: the NetworkLocalPlayer entity's moves go to the
// world server and its corrections come back; every remote player the server spawns becomes a clone of the
// NetworkReplication's template entity (with a NetworkEntity) - an NPC a clone of the child of npcTemplates named by its
// template, or of the template entity if there is none - interpolated toward each world.entityMove and destroyed
// on world.despawn / world.disconnected. Each world.component / world.componentRemoved is applied to the same-named Lua
// component on the clone or the local player (added if absent, drift rules for mismatches; a name with no Lua
// definition is a Console warning, once per name). The world client knows nothing about entities; only this unit does.
// world.entered / world.mapChanged also set every Terrain's map (terrain.h streams it).
// Positions are Transform positions: keep the local player and the template at the root (clones are created there).
namespace kast {

// --- Pure helpers (no scene, no network) ---

// Where a clone is shown elapsed seconds after it started from `from` toward `to`, arriving after duration seconds:
// a straight line, held at `to` afterwards. duration <= 0 snaps.
bx::Vec3 interpolate(const bx::Vec3& from, const bx::Vec3& to, float elapsed, float duration);

// The protocol's heading (radians, atan2(dx, dz) of the facing direction: 0 faces +Z) <-> a rotation about +Y.
bx::Quaternion yawRotation(float heading);
float headingOf(const bx::Quaternion& rotation);

// When to send the local player's Move: only when the position or heading changed since the last send, and at most
// rateHz times a second on average (a send a frame late does not push the schedule back). now is in seconds, from any
// fixed origin.
class MoveThrottle {
public:
    // True if a Move should go out now (it then counts as sent).
    bool shouldSend(const bx::Vec3& position, float heading, double now, float rateHz);
    // Takes position / heading as already known by the server (world.entered, world.correction): no Move for it.
    void known(const bx::Vec3& position, float heading);
    void reset() { *this = {}; }

private:
    std::optional<bx::Vec3> position_;
    float heading_ = 0.0f;
    std::optional<double> due_; // when the next Move may go
};

// --- ECS plumbing ---

class Replication {
public:
    // Starts on a scene (Play start, each kast.loadScene switch): when world is enabled, takes the first
    // NetworkReplication's template entity and its npcTemplates entity (with the NPC templates under it) out of the
    // scene (kept aside for cloning), subscribes to the world.* events
    // on bus and re-creates the clones of remote players already in view. Otherwise does nothing. Call it before
    // anything (physics, scripts) looks at the scene. scene, bus and world must outlive the session (until stop()).
    void start(Scene& scene, EventBus& bus, WorldClient& world);
    // Sends the local player's Move when due, then moves the clones along. dt in seconds.
    void update(float dt);
    // Ends the scene's session: unsubscribes and drops the template. remembered: keep the remote players in view (a
    // scene switch, the connection stays) or forget them (Stop).
    void stop(bool remembered = false);

    // The clone of a server entity id, entt::null if none (for tests and C++ callers).
    entt::entity clone(uint32_t entityId) const;

private:
    struct Remote {
        std::string name;
        bool npc = false;
        std::string templateName; // an NPC's template (world.spawn "template")
        bx::Vec3 from{0.0f, 0.0f, 0.0f}, to{0.0f, 0.0f, 0.0f};
        float heading = 0.0f;
        float elapsed = 0.0f;
        entt::entity entity = entt::null;
        std::map<std::string, nlohmann::json> components; // last replicated values, re-applied to a new clone
    };

    entt::entity localPlayer() const;
    float interpolationSeconds() const;
    void placeLocal(const nlohmann::json& payload);
    void setMap(const nlohmann::json& payload); // world.entered / world.mapChanged "map", then applyMap
    void applyMap();                            // every Terrain shows map_ (if known)
    // Copies every child of the entity named folderName into npcTemplates_, then takes that entity out of the scene.
    void takeNpcTemplates(Scene& scene, const std::string& folderName);
    // What to clone for remote: an NPC's own template when there is one, else the template entity (an NPC falling back
    // warns once per template name). Null scene: nothing to clone.
    std::pair<const Scene*, entt::entity> cloneSource(const Remote& remote);
    void spawn(uint32_t id, Remote remote);
    void despawn(uint32_t id);
    void despawnAll();
    void applyComponent(const nlohmann::json& event, bool removed);
    void applyComponent(entt::entity entity, const std::string& name, const nlohmann::json* data);

    Scene* scene_ = nullptr;
    EventBus* bus_ = nullptr;
    WorldClient* world_ = nullptr;
    std::optional<Scene> template_;          // holds the template's subtree (its root is the only root)
    std::optional<Scene> npcTemplateScene_;  // holds the NPC templates' subtrees, one root each
    std::map<std::string, entt::entity> npcTemplates_; // NPC template name -> its root in npcTemplateScene_
    std::unordered_map<uint32_t, Remote> remotes_;
    std::vector<EventBus::Subscription> subscriptions_;
    MoveThrottle throttle_;
    double time_ = 0.0;
    std::optional<int> map_;                                      // the map the server put the local player on
    std::optional<uint32_t> localId_;                             // the local player's server entity id
    std::map<std::string, nlohmann::json> localComponents_;        // re-applied to a new scene's local player
    std::set<std::string> warned_;                                // unknown names and drift fixes, warned once
};

} // namespace kast
