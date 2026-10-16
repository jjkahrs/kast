// The zone: owns the ECS registry, the AOI grid and the Lua systems, and runs the systems in a fixed order each tick.
// Talks to the outside only through Send (to a session) and Save (to the DB worker).
#pragma once
#include "systems.h"

#include <db/char_repo.h>
#include <lua/runtime.h>

#include <filesystem>
#include <memory>

#include <entt/entity/registry.hpp>

namespace kast::world {

struct WorldRules {
    MoveRules move;
    ChatRules chat;
    float cellSize = 64, viewRadius = 128;
    proto::Vec3 spawn{1024, 0, 1024};
    size_t chatRateCount = 5;
    Millis chatRateWindow{10000};
    Millis saveInterval{60000};
    float tickSeconds = 0.05f; // dt passed to Lua update
};

class World {
public:
    // defs: the Lua component types (lua/components.h); their instances are restored, saved and replicated here.
    World(WorldRules rules, Send send, Save save, lua::Defs defs = {});

    // Loads the state machine definitions under luaRoot/fsms/ and the Lua systems under luaRoot/systems/, runs their
    // init, then starts the game machines (by name); every error, empty if all loaded.
    std::vector<std::string> load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines = {});
    // Graceful shutdown, before the despawns: onLeave for every character, then every system's shutdown, then the
    // client events they queued. Undelivered bus events are dropped.
    void shutdown_scripts();
    std::vector<lua::SystemStatus> script_status() const { return lua_.status(); }
    EventBus& bus() { return lua_.bus(); } // the Lua systems' event bus, for server C++
    lua::StateMachines& fsm() { return lua_.fsm(); } // the game and character state machines, for server C++

    // Places the character (at its saved position, or the spawn point if that is outside the zone), sends
    // EnterWorldOk, restores its persisted Lua components and runs onEnter. Other players get Spawn on the next tick.
    entt::entity spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now);
    // Runs onLeave and sends the client events it queued, saves the character, sends Despawn to everyone who could
    // see it, removes it.
    void despawn(entt::entity e);
    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, std::string text);

    void tick(TimePoint now);
    size_t population() const { return population_; }
    entt::registry& registry() { return reg_; } // for tests

private:
    WorldRules rules_;
    Send send_;
    Save save_;
    lua::Defs defs_;
    entt::registry reg_;
    AoiGrid grid_;
    lua::Runtime lua_; // after reg_: it holds a reference
    size_t population_ = 0;
};

} // namespace kast::world
