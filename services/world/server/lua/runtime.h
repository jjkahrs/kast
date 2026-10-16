// Server Lua systems: LUA_ROOT/systems/*.lua, run in alphabetical filename order on the tick thread, one Lua state,
// each file in its own sandboxed environment. A file returns { init?, update(dt), shutdown?, onEnter?(e), onLeave?(e) }
// and reaches the world only through kast.* over the registry, the event bus and the state machines (README "Lua
// components and systems").
// sol2 stays inside runtime.cpp: World holds a Runtime without seeing Lua types.
#pragma once
#include "components.h"
#include "event_bus.h"
#include "state_machines.h"

#include <map.h>
#include <messages.h>

#include <entt/entity/registry.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kast::lua {

struct SystemStatus {
    std::string path; // LUA_ROOT-relative
    bool ok = false;
    std::string error; // why it is disabled ("path:line: ..."), empty when ok
};

// A kast.sendTo/sendNearby/broadcast call, queued until World sends it as ServerEvent.
struct ClientEvent {
    enum class To { owner, nearby, all };
    To to = To::all;
    uint32_t entity = 0; // wire id; unused for all
    std::string name;
    std::string json;
};

// How kast.create / kast.destroy / kast.transfer / an NPC Transform set reach the world, which owns the maps, the AOI
// grid and the sends.
// Runtime checks every argument first; these only run for valid ones.
struct NpcHooks {
    std::function<entt::entity(std::string name, std::string templateName, std::optional<uint16_t> map,
                               std::optional<proto::Vec3> pos, float heading)>
        create;                                               // map empty: START_MAP; pos empty: its spawn point
    std::function<void(entt::entity)> destroy;                // an existing NPC
    std::function<proto::Vec3(uint16_t map, proto::Vec3)> clamp; // onto the (loaded) map's ground, within MIN_Y/MAX_Y
    std::function<const map::MapInfo*(uint16_t id)> map;      // nullptr: not loaded
    std::function<std::optional<float>(uint16_t map, float x, float z)> ground; // Maps::ground; nullopt: off the map
    std::function<std::vector<proto::Vec3>(uint16_t map, proto::Vec3 from, proto::Vec3 to)> path; // Nav::path
    std::function<bool(entt::entity, uint16_t map, proto::Vec3 pos, float heading)> transfer; // World::transfer
    std::function<const map::Spawn*(uint32_t record)> spawn;  // a SpawnRef's spawn file entry
    size_t max = 0;                                           // MAX_NPCS
};

class Runtime {
public:
    Runtime(entt::registry& reg, Defs defs, NpcHooks npcs = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Loads the state machine definitions under root/fsms/, then every system under root/systems/ and runs each init,
    // then the game machines (gameMachines, by name) enter their initial state. Returns every error (a rejected
    // definition, a listed machine without one, a bad system); non-empty means the server must not start. A missing
    // folder is no machines / no systems.
    std::vector<std::string> load(const std::filesystem::path& root, std::vector<std::string> gameMachines = {});
    // A runtime error in any of these disables only that system (logged with path:line) until restart.
    void update(float dt);
    void enter(entt::entity e); // onEnter: after the character spawned and its persisted components were restored
    void leave(entt::entity e); // onLeave: before it is saved and despawned
    void shutdown(); // also drops the events still queued on the bus
    std::vector<SystemStatus> status() const;

    // Delivers the bus events queued so far (see EventBus::drain). A failing Lua subscriber disables its system.
    void drain_events();
    EventBus& bus();
    StateMachines& fsm();
    // The client events queued since the last call, in call order.
    std::vector<ClientEvent> take_client_events();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kast::lua
