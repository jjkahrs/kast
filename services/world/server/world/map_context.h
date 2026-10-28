// One loaded map's runtime (docs/services/world/maps.html#map-contexts): its own registry, AOI grid, Lua state (npcs/
// and systems/), spawner, tile activation and event bus, updated by World. A context reads only the immutable Maps
// and the process-wide Shared; whatever reaches past its map (a transfer, kast.create or a respawn on another map,
// kast.world.post, a game machine change, a broadcast) goes into its Outbox, which World drains at the serial points.
#pragma once
#include "maps.h"
#include "systems.h"

#include <db/char_repo.h>
#include <lua/runtime.h>

#include <filesystem>
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

// What every context shares, owned by World. Entity ids come from one process-wide pool: a context creates each entity
// with an id from it (registry::create(hint)), so an entt::entity, and the wire id it is, is unique across contexts,
// and an entity keeps it when it moves to another context. Ids are handed out and recycled in the order one registry
// would.
// ponytail: plain members, touched only on the tick thread; the parallel map update (phase 3) needs a lock or a
// per-context id range here.
struct Shared {
    entt::storage<entt::entity> ids;
    size_t npcs = 0; // in every context (and on their way to one), for MAX_NPCS
};

class MapContext {
public:
    // Work for other contexts, queued during this one's update and done by World at the next serial point.
    struct Create { // kast.create onto another map; id and its count in Shared are taken already
        entt::entity id;
        std::string name, templateName;
        uint16_t map;
        std::optional<proto::Vec3> pos;
        float heading;
        std::string npc;
        lua::Json components;
    };
    struct Outbox {
        std::vector<std::pair<entt::entity, Destination>> transfers; // onto another map (a portal, kast.transfer)
        std::vector<Create> creates;
        std::vector<std::pair<uint16_t, uint32_t>> respawns; // (map, record): its spawn's NPC was destroyed here
        std::vector<std::pair<std::string, std::string>> gameChanges; // (machine, to): kast.fsm.change on a game machine
        std::vector<lua::ClientEvent> broadcasts; // kast.broadcast: sent here already, for the other contexts' players
        std::vector<lua::Post> posts;
        bool empty() const {
            return transfers.empty() && creates.empty() && respawns.empty() && gameChanges.empty() && broadcasts.empty() &&
                   posts.empty();
        }
    };

    // rules, maps (which holds map) and shared must outlive the context. now: the current tick's time.
    MapContext(uint16_t map, const WorldRules& rules, const Maps& maps, Shared& shared, Send send, SendMany sendMany,
               Save save, lua::Defs defs, TimePoint now);

    uint16_t map() const { return map_; }
    // lua::Runtime::load, with the game machines' states (the realm's) mirrored first, so an init already reads them;
    // then every spawn file entry's "npc" must have a definition. Every error.
    std::vector<std::string> load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines,
                                          const std::map<std::string, std::string>& gameStates);
    void shutdown_scripts(); // as World::shutdown_scripts, for this context
    std::vector<lua::SystemStatus> script_status() const { return lua_.status(); }
    bool validate_character(const std::string& name, const proto::CharOptions& options) {
        return lua_.validate_character(name, options);
    }
    EventBus& bus() { return lua_.bus(); }
    lua::StateMachines& fsm() { return lua_.fsm(); }
    entt::registry& registry() { return reg_; }
    size_t population() const { return population_; }

    // As World's, for an entity of (or a position on) this map. spawn: pos already checked (on the map, on its ground).
    entt::entity spawn(uint32_t sessionId, const db::CharRow& row, proto::Vec3 pos, float heading, TimePoint now);
    void despawn(entt::entity e);
    // id: one Shared already counts (a queued Create), else a new one.
    entt::entity create_npc(std::string name, std::string templateName, std::optional<proto::Vec3> pos, float heading,
                            std::string npc, const lua::Json& components, entt::entity id = entt::null);
    void destroy_npc(entt::entity e);
    void remove_npcs();
    // A transfer within this map, as today's World::transfer (the destination is owned).
    void transfer(entt::entity e, const Destination& to);
    // The NPC of this map's spawn record died in another context: it respawns as if destroyed here.
    void spawn_destroyed(uint32_t record);

    // Moving e to another context, in this order (World::transfer): leave_map runs onMapLeave (and sends the client
    // events queued so far, while e can still be seen); depart takes e out of this map (MapChange, Despawns,
    // area.leave, its grid cell and tiles) but leaves its components; World moves them over; the destination's
    // arrive puts it on that map (grid, tiles, area.enter) and enter_map runs onMapEnter.
    lua::Carried leave_map(entt::entity e);
    void depart(entt::entity e, const Destination& to);
    void arrive(entt::entity e, const Destination& to);
    void enter_map(entt::entity e, const lua::Carried& carried) { lua_.enter_map(e, carried); }

    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, proto::Say say);
    void push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId);
    void push_interact(entt::entity e, uint32_t objectId);
    // Client events from another context (the realm's, another map's broadcast) for this one's players.
    void send_client_events(std::vector<lua::ClientEvent> events);

    // The tick, split at World's serial point: movement and areas (portal transfers onto another map are queued), then
    // the rest (World::tick lists the order).
    void tick_move(TimePoint now);
    void tick_rest(TimePoint now, const Mutes& mutes);
    Outbox take_outbox() { return std::exchange(outbox_, {}); }
    // The realm's game machine states, for kast.fsm here.
    void mirror_game_machines(const std::map<std::string, std::string>& states) {
        if (lua_.fsm().game_states() != states) lua_.fsm().mirror(states);
    }

private:
    // A transfer from Lua or a portal: refused (false, logged) if this process does not own the destination tile;
    // within this map at once, onto another queued.
    bool request_transfer(entt::entity e, const Destination& to);
    entt::entity lua_create(std::string name, std::string templateName, std::optional<uint16_t> map,
                            std::optional<proto::Vec3> pos, float heading, std::string npc, const lua::Json& components);
    void leave_views(entt::entity e);
    void spawn_due(TimePoint now);
    proto::Vec3 clamp(proto::Vec3 pos) const;
    // Sends the client events Lua queued; a broadcast also goes to the outbox for the other contexts.
    void send_lua_client_events();
    entt::entity create(entt::entity id); // in the registry with id, one from Shared
    void destroy(entt::entity e);         // out of the registry, its id back to Shared

    uint16_t map_;
    const WorldRules& rules_;
    const Maps& maps_;
    Shared& shared_;
    Send send_;
    SendMany sendMany_;
    Save save_;
    lua::Defs defs_;
    entt::registry reg_;
    AoiGrid grid_;
    lua::Runtime lua_; // after reg_ and grid_: it holds a reference and creates NPCs through this context
    Spawner spawner_;  // every map's spawn file entries, in Maps order (SpawnRef indexes it); only this map's spawn here
    Activation activation_;
    Outbox outbox_;
    size_t population_ = 0;
    TimePoint now_{}; // the current tick's time (transfer resets move budgets to it)
};

// Moves every component of e (the C++ ones and every Lua component, persisted or not, StateMachine included) from one
// registry to another, where e is created with the same id; e is gone from `from` afterwards. defs: the Lua
// component types.
void move_entity(entt::registry& from, entt::registry& to, entt::entity e, const lua::Defs& defs);
// pos on a declared tile of map (which must be loaded), standing on its ground (within MIN_Y/MAX_Y).
proto::Vec3 clamp_to_map(const Maps& maps, const MoveRules& rules, uint16_t map, proto::Vec3 pos);

} // namespace kast::world
