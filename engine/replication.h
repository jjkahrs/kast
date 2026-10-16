#pragma once

#include "event_bus.h"
#include "scene.h"
#include "world_client.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Replication: networked players without sync code (README "Online Services → Replication"). Consumes the world
// client's "world.*" events from the event bus and drives the scene: the NetworkLocalPlayer entity's moves go to the
// world server and its corrections come back; every remote player the server spawns becomes a clone of the
// NetworkReplication's template entity (with a NetworkEntity), interpolated toward each world.entityMove and destroyed
// on world.despawn / world.disconnected. The world client knows nothing about entities; only this unit does.
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
    // NetworkReplication's template entity out of the scene (kept aside for cloning), subscribes to the world.* events
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
        bx::Vec3 from{0.0f, 0.0f, 0.0f}, to{0.0f, 0.0f, 0.0f};
        float heading = 0.0f;
        float elapsed = 0.0f;
        entt::entity entity = entt::null;
    };

    entt::entity localPlayer() const;
    float interpolationSeconds() const;
    void placeLocal(const nlohmann::json& payload);
    void spawn(uint32_t id, Remote remote);
    void despawn(uint32_t id);
    void despawnAll();

    Scene* scene_ = nullptr;
    EventBus* bus_ = nullptr;
    WorldClient* world_ = nullptr;
    std::optional<Scene> template_;          // holds the template's subtree (its root is the only root)
    std::unordered_map<uint32_t, Remote> remotes_;
    std::vector<EventBus::Subscription> subscriptions_;
    MoveThrottle throttle_;
    double time_ = 0.0;
};

} // namespace kast
