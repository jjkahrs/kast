#pragma once

#include "audio.h"
#include "event_bus.h"
#include "input.h"
#include "physics.h"
#include "scene.h"
#include "state_machines.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Lua systems: global ECS systems written in Lua, listed in Scene::systems and run once per frame in Play mode after
// the C++ systems. One LuaJIT state per Play session; each file runs in its own sandboxed environment. See README
// "Lua systems" for the file shape and the kast.* API.
namespace kast {

struct LuaSystemStatus {
    std::string path;  // as listed in Scene::systems
    bool ok = false;   // false: disabled until the next start() or a successful hot reload
    std::string error; // last error, empty when ok
};

class LuaSystems {
public:
    LuaSystems();
    ~LuaSystems();
    LuaSystems(const LuaSystems&) = delete;
    LuaSystems& operator=(const LuaSystems&) = delete;

    // Starts a session: resets the event bus (queue, subscriptions, history) and the state machines, loads the
    // project's state machine definitions (fsms/) and every script in scene.systems (paths relative to projectRoot),
    // calls each init, then enters gameMachines (project.kast "stateMachines") and every StateMachine entity.
    // With no project every listed system is disabled ("no project"). scenePath is the scene's project-relative
    // path, empty if unsaved or outside a project (kast.currentScene returns nil). Stops any running session first
    // (without its shutdowns: the old scene may be gone). scene and input must outlive the session; input may be
    // null (no keys down, no mouse motion).
    void start(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath,
               const Input* input, std::vector<std::string> gameMachines = {});
    // Reloads scripts and state machine definitions whose file changed, enters the machines of entities that gained
    // StateMachine, then calls each enabled system's update(dt) in listed order, then drains the event bus. Then
    // applies a scene switch queued by kast.loadScene: the old systems' shutdowns run, queued events are dropped, the
    // loaded scene replaces scene (same object) and a new session starts for its systems (C++ subscriptions, the bus
    // history and the game state machines' states are kept). Returns true if the scene was switched.
    bool tick(float dt);
    // Calls each enabled system's shutdown, ends the session, resets the state machines and drops queued events.
    // Does nothing if none is running.
    void stop();

    bool running() const { return state_ != nullptr; }
    // One entry per Scene::systems entry while running; empty otherwise.
    std::vector<LuaSystemStatus> status() const;
    // The running scene's project-relative path; empty if unsaved, outside a project or no session is running.
    std::string scenePath() const;

    // The session's event bus, also for C++ publishers and subscribers. Its history outlives stop().
    EventBus& bus() { return bus_; }
    const EventBus& bus() const { return bus_; }
    // The session's physics world (kast.physics.* from C++): started with the session (and on a scene switch),
    // stopped with it. tickPlay steps it.
    Physics& physics() { return physics_; }
    const Physics& physics() const { return physics_; }
    // The session's state machines (kast.fsm.* from C++). Usable while running; reset by start() and stop().
    // The session's sound (kast.audio.* from C++): start() stops every voice and resets the buses to 1.0 (apply the
    // project's defaults after it); a scene switch and stop() stop every voice; AudioSources with playOnStart start
    // with each scene. tick() frees ended voices.
    Audio& audio() { return audio_; }
    const Audio& audio() const { return audio_; }
    StateMachines& fsm() { return fsm_; }
    const StateMachines& fsm() const { return fsm_; }

private:
    struct State;
    void launch(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath,
                const Input* input);
    void endSession(); // shutdowns, then the Lua state goes; the state machines keep their game states

    EventBus bus_; // declared before state_: the systems' subscriptions unsubscribe from it when state_ goes
    Physics physics_; // publishes on bus_
    Audio audio_;
    std::unique_ptr<State> state_;
    StateMachines fsm_{bus_}; // declared after state_, so its Lua callbacks are released while the Lua state lives
};

} // namespace kast
