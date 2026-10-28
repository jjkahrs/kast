// The world: every loaded map (maps.h) in one ECS registry and one AOI grid, plus the Lua systems and the NPCs of the
// maps' spawn files (spawner_system), spawned per tile as players come near and asleep away from them (Activation);
// runs the systems in a fixed order each tick. Talks to the outside only through Send (to a session), SendMany (one message to several) and Save (to the DB worker).
// Each player and NPC is on one map (MapRef); only transfer() changes it.
#pragma once
#include "maps.h"
#include "systems.h"

#include <db/char_repo.h>
#include <lua/runtime.h>

#include <filesystem>
#include <memory>
#include <optional>

#include <entt/entity/registry.hpp>

namespace kast::world {

struct WorldRules {
    MoveRules move;
    ChatRules chat;
    float cellSize = 64, viewRadius = 128;
    uint16_t startMap = 1; // START_MAP: new characters, NPCs without a map, and the fallback for a bad saved map
    size_t chatRateCount = 5;
    Millis chatRateWindow{10000};
    size_t clientEventRateCount = 10; // kast.world.send events per character per window; the rest are dropped
    Millis clientEventRateWindow{1000};
    Millis saveInterval{60000};
    float tickSeconds = 0.05f; // dt passed to Lua update
    size_t maxNpcs = 10000;    // kast.create raises past this
    int activationTiles = 1;   // ACTIVATION_TILES: tiles around a player's that load and stay awake (Activation)
};

// Where a transfer puts an entity.
struct Destination {
    uint16_t map = 0;
    proto::Vec3 pos;
    float heading = 0;
};

class World {
public:
    // maps must outlive the World and hold rules.startMap. defs: the Lua component types (lua/components.h); their
    // instances are restored, saved and replicated here.
    World(WorldRules rules, const Maps& maps, Send send, SendMany sendMany, Save save, lua::Defs defs = {});

    // Loads the state machine definitions under luaRoot/fsms/, the NPC definitions under luaRoot/npcs/ and the Lua
    // systems under luaRoot/systems/, runs their init, then starts the game machines (by name); every error, empty if
    // all loaded. A spawn whose "npc" has no definition is one too.
    std::vector<std::string> load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines = {});
    // Graceful shutdown, before the despawns: onLeave for every character, then every system's shutdown, then the
    // client events they queued. Undelivered bus events are dropped.
    void shutdown_scripts();
    std::vector<lua::SystemStatus> script_status() const { return lua_.status(); }
    // The Lua systems' validateCharacter verdict on C++-checked options (lua::Runtime::validate_character).
    bool validate_character(const std::string& name, const proto::CharOptions& options) {
        return lua_.validate_character(name, options);
    }
    EventBus& bus() { return lua_.bus(); } // the Lua systems' event bus, for server C++
    lua::StateMachines& fsm() { return lua_.fsm(); } // the game and character state machines, for server C++

    // Places the character (at its saved map and position, lifted onto the ground if it is under it, or START_MAP's
    // spawn if that map is not loaded or the position is off it), sends
    // EnterWorldOk, wakes the tiles around it (spawning the NPCs of those loading now, so onEnter sees them), restores
    // its persisted Lua components and runs onEnter. Other players get Spawn on the next tick.
    entt::entity spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now);
    // Runs onLeave and sends the client events it queued, saves the character, sends Despawn to everyone who could
    // see it, removes it.
    void despawn(entt::entity e);
    // An NPC (kast.create, a spawn) on map (default START_MAP, must be loaded) at pos (clamp_to_map: on the ground), or
    // the map's spawn point (on the ground too), made from definition npc ("" for none) with components ({"<Type>":
    // valid value}; a StateMachine enters its initial state now). Players in range get its Spawn on the next tick. It
    // has no session and is never saved.
    entt::entity create_npc(std::string name, std::string templateName, std::optional<uint16_t> map,
                            std::optional<proto::Vec3> pos, float heading, std::string npc = "",
                            const lua::Json& components = lua::Json::object());
    // Sends Despawn to every player who sees the NPC, removes it. One from a spawn file (SpawnRef) comes back
    // respawnSec after this (never for -1), asleep or not.
    void destroy_npc(entt::entity e);
    // Graceful shutdown, after the players are gone: removes every NPC; nothing to send, nothing to save.
    void remove_npcs();
    // pos on a declared tile of map (which must be loaded), standing on its ground (within MIN_Y/MAX_Y).
    proto::Vec3 clamp_to_map(uint16_t map, proto::Vec3 pos) const;
    // Every map change goes through here (player or NPC), and teleports within a map too. Refused (false, logged) if
    // this process does not own the destination tile. Otherwise moves e at once onto the ground at the destination and takes it out of every view: the
    // player gets MapChange, then a Despawn per old neighbour, who get its Despawn, and its area is updated (area
    // events). The next aoi pass sends the Spawns for the new position: this tick's for a portal (area_system runs
    // before aoi), the next tick's for kast.transfer.
    bool transfer(entt::entity e, const Destination& to);

    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, proto::Say say);
    // The chat service's mutes (via the chat link): chat_system drops a muted character's says, yells and emotes.
    Mutes& mutes() { return mutes_; }
    // The map e is on (MapRef).
    uint16_t map_of(entt::entity e) const;
    // A ClientEvent GameServer already checked (valid name, JSON object within kClientEventJsonMax).
    void push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId = 0);
    // An Interact; checked by interact_system on the next tick.
    void push_interact(entt::entity e, uint32_t objectId);

    void tick(TimePoint now);
    size_t population() const { return population_; }
    entt::registry& registry() { return reg_; } // for tests

private:
    // Takes e out of every view, both ways: Despawn to each player that sees it, e's own view links emptied.
    void leave_views(entt::entity e);
    // spawner_system: creates the NPCs of the due spawn records.
    void spawn_due(TimePoint now);

    WorldRules rules_;
    const Maps& maps_;
    Send send_;
    SendMany sendMany_;
    Save save_;
    lua::Defs defs_;
    entt::registry reg_;
    AoiGrid grid_;
    lua::Runtime lua_; // after reg_ and grid_: it holds a reference and creates NPCs through this World
    Mutes mutes_;
    Spawner spawner_; // every loaded spawn file entry, in Maps order; SpawnRef indexes it
    Activation activation_;
    size_t population_ = 0;
    TimePoint now_{}; // the current tick's time (transfer resets move budgets to it)
};

} // namespace kast::world
