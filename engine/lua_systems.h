#pragma once

#include "event_bus.h"
#include "input.h"
#include "scene.h"

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

    // Starts a session: resets the event bus (queue, subscriptions, history), loads every script in scene.systems
    // (paths relative to projectRoot) and calls each init.
    // With no project every listed system is disabled ("no project"). scenePath is the scene's project-relative
    // path, empty if unsaved or outside a project (kast.currentScene returns nil). Stops any running session first
    // (without its shutdowns: the old scene may be gone). scene and input must outlive the session; input may be
    // null (no keys down, no mouse motion).
    void start(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath,
               const Input* input);
    // Reloads scripts whose file changed, then calls each enabled system's update(dt) in listed order, then drains
    // the event bus. Then applies a scene switch queued by kast.loadScene: the old systems' shutdowns run, queued
    // events are dropped, the loaded scene replaces scene (same object) and a new session starts for its systems (C++
    // subscriptions and the bus history are kept). Returns true if the scene was switched.
    bool tick(float dt);
    // Calls each enabled system's shutdown, ends the session and drops queued events. Does nothing if none is
    // running.
    void stop();

    bool running() const { return state_ != nullptr; }
    // One entry per Scene::systems entry while running; empty otherwise.
    std::vector<LuaSystemStatus> status() const;
    // The running scene's project-relative path; empty if unsaved, outside a project or no session is running.
    std::string scenePath() const;

    // The session's event bus, also for C++ publishers and subscribers. Its history outlives stop().
    EventBus& bus() { return bus_; }
    const EventBus& bus() const { return bus_; }

private:
    struct State;
    void launch(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath,
                const Input* input);

    EventBus bus_; // declared before state_: the systems' subscriptions unsubscribe from it when state_ goes
    std::unique_ptr<State> state_;
};

} // namespace kast
