// Server Lua systems: LUA_ROOT/systems/*.lua, run in alphabetical filename order on the tick thread, one Lua state,
// each file in its own sandboxed environment. A file returns { init?, update(dt), shutdown?, onEnter?(e), onLeave?(e) }
// and reaches the world only through kast.* over the registry (README "Lua components and systems").
// sol2 stays inside runtime.cpp: World holds a Runtime without seeing Lua types.
#pragma once
#include "components.h"

#include <entt/entity/registry.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kast::lua {

struct SystemStatus {
    std::string path; // LUA_ROOT-relative
    bool ok = false;
    std::string error; // why it is disabled ("path:line: ..."), empty when ok
};

class Runtime {
public:
    Runtime(entt::registry& reg, Defs defs);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Loads every system under root/systems/ and runs each init. Returns every error; non-empty means the server must
    // not start. A missing folder is no systems.
    std::vector<std::string> load(const std::filesystem::path& root);
    // A runtime error in any of these disables only that system (logged with path:line) until restart.
    void update(float dt);
    void enter(entt::entity e); // onEnter: after the character spawned and its persisted components were restored
    void leave(entt::entity e); // onLeave: before it is saved and despawned
    void shutdown();
    std::vector<SystemStatus> status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kast::lua
