#pragma once

#include "input.h"
#include "scene.h"

#include <filesystem>
#include <memory>
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

    // Starts a session: loads every script in scene.systems (paths relative to baseDir) and calls each init.
    // Stops any running session first (without its shutdowns: the old scene may be gone). scene and input must
    // outlive the session; input may be null (no keys down, no mouse motion).
    void start(Scene& scene, const std::filesystem::path& baseDir, const Input* input);
    // Reloads scripts whose file changed, then calls each enabled system's update(dt) in listed order.
    void tick(float dt);
    // Calls each enabled system's shutdown and ends the session. Does nothing if none is running.
    void stop();

    bool running() const { return state_ != nullptr; }
    // One entry per Scene::systems entry while running; empty otherwise.
    std::vector<LuaSystemStatus> status() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace kast
