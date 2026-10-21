#pragma once

#include "event_bus.h"
#include "scene.h"
#include "world_client.h"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Replication: networked players without sync code (docs/engine/online-services.html#replication). Consumes the world
// client's "world.*" events from the event bus and drives the scene: the NetworkLocalPlayer entity's moves go to the
// world server and its corrections come back, reconciled with the moves still in flight (MoveHistory) and blended in
// when small (CorrectionBlend); every remote
// player the server spawns becomes a clone of the NetworkReplication's template entity (with a NetworkEntity) - an
// NPC a clone of the child of npcTemplates named by its
// template, or of the template entity if there is none - interpolated toward each world.entityMove (then
// extrapolated past it for a while, RemoteMotion) and destroyed
// on world.despawn / world.disconnected. Each world.component / world.componentRemoved is applied to the same-named Lua
// component on the clone or the local player (added if absent, drift rules for mismatches; a name with no Lua
// definition is a Console warning, once per name). The world client knows nothing about entities; only this unit does.
// world.entered / world.mapChanged also set every Terrain's map (terrain.h streams it).
// A kast.loadScene switch keeps the map, the local player's id, pose and components and the remote players in view: the
// new scene's Terrain, local player and clones pick them up, even after a scene with no local player (character select),
// and its scripts get world.entered (the player's map and pose now), world.entitySpawned and world.component again.
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

// Reconciliation of the local player's Moves with the server's Corrections (docs/services/world/protocol.html#movement):
// the Moves sent since the last placement, as position deltas by seq. A Correction{position, ackSeq, epoch} means
// the server has the player at position after Move ackSeq and ignores every later Move of an older epoch; the client
// is then at position + the deltas it sent after ackSeq + the one not sent yet (rebase). Position-delta replay only:
// no inputs are recorded and nothing is re-simulated.
class MoveHistory {
public:
    static constexpr size_t kCapacity = 64; // 6.4 s at 10 Hz: far more than a round trip, even a lagged one

    struct Stamp {
        uint32_t seq = 0;
        uint32_t epoch = 0;
    };
    // A Move to position goes out now: records its delta, returns its seq (from 1) and the current epoch.
    Stamp sent(const bx::Vec3& position);
    // The server placed the player there (world.entered, world.mapChanged): later deltas start from it. seq goes on (a
    // Correction for a Move from before the placement acks a seq older than every delta: they all replay); so does the
    // epoch, unless the placement brings one (world.mapChanged: the Moves sent before it are dropped by the server).
    void placed(const bx::Vec3& position, std::optional<uint32_t> epoch = std::nullopt);
    // A Correction while the player is at current: the rebased position (see above). Adopts epoch for later Moves,
    // drops the acked deltas, and the next delta starts from the rebased position.
    bx::Vec3 rebase(const bx::Vec3& position, uint32_t ackSeq, uint32_t epoch, const bx::Vec3& current);
    // A new character or connection: seq and epoch from 0 (the server's are new too).
    void reset() { *this = {}; }

    uint32_t epoch() const { return epoch_; }
    size_t size() const { return deltas_.size(); }

private:
    struct Delta {
        uint32_t seq;
        bx::Vec3 delta;
    };
    std::deque<Delta> deltas_;
    std::optional<bx::Vec3> last_; // the last sent or placed position: where the unsent delta starts
    uint32_t seq_ = 0;
    uint32_t epoch_ = 0;
};

// Smoothing of the local player's corrections: a reconciled error under the threshold is not snapped away but blended
// out of the shown position over a duration while the player keeps moving; at or over it (or duration <= 0) it snaps.
// The Transform shows the true position + offset(); Moves carry the true position, so the blend never reaches the
// server as extra speed.
class CorrectionBlend {
public:
    // A Correction rebased the true position to `rebased` while `shown` is displayed: true if it snaps (show rebased),
    // false if shown stays and the difference blends out over duration seconds (a blend in progress restarts from it).
    bool correct(const bx::Vec3& shown, const bx::Vec3& rebased, float threshold, float duration);
    // Advances dt seconds: what to add to the shown position this frame.
    bx::Vec3 step(float dt);
    // Shown minus true position now; zero once blended.
    bx::Vec3 offset() const;
    void reset() { *this = {}; }

private:
    bx::Vec3 error_{0.0f, 0.0f, 0.0f};
    float elapsed_ = 0.0f;
    float duration_ = 0.0f;
};

// Where a clone is shown between the server's EntityMoves (dead reckoning): from each one it keeps going along the
// observed velocity (the distance its EntityMoves covered over the time they took, weighted to the last ~0.5 s so late,
// early or bunched ones do not jerk it; ramping up from a spawn) for up to maxExtrapolation seconds, then holds there.
// Nothing for maxExtrapolation more means the mover stopped (EntityMoves only come for moves): the clone settles back
// onto the last server position over the duration. The gap between that path and what was shown when the EntityMove
// came is blended out (no pop back) over the duration, or longer at speed (up to 0.5 s, at most 0.35 x the velocity on
// top), so a clone on the move never runs much faster than its mover, or backward. The first EntityMove after a place
// has no velocity: a plain interpolation. Heading turns the short way round over the duration instead of snapping.
class RemoteMotion {
public:
    // Shown at position / heading, standing (world.spawn, a scene switch): no velocity yet.
    void place(const bx::Vec3& position, float heading);
    // An EntityMove to position / heading, with the clone's current settings (duration, maxExtrapolation in seconds).
    void moved(const bx::Vec3& position, float heading, float duration, float maxExtrapolation);
    void step(float dt) { elapsed_ += dt; }
    bx::Vec3 position(float duration, float maxExtrapolation) const;
    float heading(float duration) const;

private:
    bx::Vec3 to_{0.0f, 0.0f, 0.0f};       // the last server position
    bx::Vec3 velocity_{0.0f, 0.0f, 0.0f}; // m/s: distance_ / time_
    bx::Vec3 distance_{0.0f, 0.0f, 0.0f}; // covered by the EntityMoves, older ones fading out
    float time_ = 0.0f;                   // the time they took, fading the same
    bx::Vec3 error_{0.0f, 0.0f, 0.0f};    // shown minus the path when the last EntityMove came, blended out
    float converge_ = 0.0f;               // over this many seconds
    float headingFrom_ = 0.0f, heading_ = 0.0f;
    float elapsed_ = 0.0f; // since the last place / moved
    bool moving_ = false;  // an EntityMove since the last place: the next one gives a velocity
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
        RemoteMotion motion;
        std::optional<float> shownHeading; // last heading written to the clone: a script's rotation stays until it turns
        entt::entity entity = entt::null;
        std::map<std::string, nlohmann::json> components; // last replicated values, re-applied to a new clone
    };

    entt::entity localPlayer() const;
    float interpolationSeconds() const;
    float extrapolationSeconds() const;
    void showRemote(Remote& remote); // the clone's Transform at remote.motion's pose
    void placeLocal(const nlohmann::json& payload); // remembers the pose, then placePlayer
    void reconcile(const nlohmann::json& payload);  // world.correction: the rebased position (MoveHistory)
    void placePlayer();                             // the scene's local player at localPose_ (if known)
    void setMap(const nlohmann::json& payload); // world.entered / world.mapChanged "map", then applyMap
    void applyMap();                            // every Terrain shows map_ (if known)
    // Copies every child of the entity named folderName into npcTemplates_, then takes that entity out of the scene.
    void takeNpcTemplates(Scene& scene, const std::string& folderName);
    // What to clone for remote: an NPC's own template when there is one, else the template entity (an NPC falling back
    // warns once per template name). Null scene: nothing to clone.
    std::pair<const Scene*, entt::entity> cloneSource(const Remote& remote);
    void spawn(uint32_t id, Remote remote, const bx::Vec3& position, float heading); // shown there, standing
    void despawn(uint32_t id);
    void despawnAll();
    void applyComponent(const nlohmann::json& event, bool removed);
    void applyComponent(entt::entity entity, const std::string& name, const nlohmann::json* data);
    // world.component again for each remembered value of server entity id (a scene switch's new scripts).
    void publishComponents(uint32_t id, const std::map<std::string, nlohmann::json>& components);

    Scene* scene_ = nullptr;
    EventBus* bus_ = nullptr;
    WorldClient* world_ = nullptr;
    std::optional<Scene> template_;          // holds the template's subtree (its root is the only root)
    std::optional<Scene> npcTemplateScene_;  // holds the NPC templates' subtrees, one root each
    std::map<std::string, entt::entity> npcTemplates_; // NPC template name -> its root in npcTemplateScene_
    std::unordered_map<uint32_t, Remote> remotes_;
    std::vector<EventBus::Subscription> subscriptions_;
    MoveThrottle throttle_;
    MoveHistory history_; // kept across scene switches: the connection and its Moves in flight stay
    CorrectionBlend blend_; // the local player's shown position = true position + blend_.offset()
    double time_ = 0.0;
    std::optional<int> map_;                                      // the map the server put the local player on
    std::optional<uint32_t> localId_;                             // the local player's server entity id
    std::map<std::string, nlohmann::json> localComponents_;        // re-applied to a new scene's local player
    struct Pose {
        bx::Vec3 position{0.0f, 0.0f, 0.0f};
        bx::Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    };
    std::optional<Pose> localPose_; // where the local player is (server placement, then each sent frame)
    std::set<std::string> warned_;                                // unknown names and drift fixes, warned once
};

} // namespace kast
