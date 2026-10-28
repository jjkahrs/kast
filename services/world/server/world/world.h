// The world: one MapContext per loaded map (maps.h), each with its own registry, AOI grid, Lua systems, NPC spawns and
// tile activation, created the first time a map is used (START_MAP's when the scripts load), plus the realm context,
// whose Lua state runs only the game state machines. Updates them serially on the tick thread, with a serial point
// between the steps where the work the contexts queued for one another is done (docs/services/world/maps.html#map-contexts).
// Talks to the outside only through Send (to a session), SendMany (one message to several) and Save (to the DB
// worker). Each player and NPC is on one map (MapRef), in that map's context; only transfer() changes it. Entity ids
// are process-wide (Shared), so the ids on the wire and in Lua stay unique and stable across contexts.
#pragma once
#include "map_context.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace kast::world {

class World {
public:
    // maps must outlive the World and hold rules.startMap. defs: the Lua component types (lua/components.h); their
    // instances are restored, saved and replicated here.
    World(WorldRules rules, const Maps& maps, Send send, SendMany sendMany, Save save, lua::Defs defs = {});
    ~World();

    // Loads the state machine definitions under luaRoot/fsms/ and starts the game machines (by name) in the realm
    // context, then, in every map context (START_MAP's, created now, and any created later), fsms/, the NPC
    // definitions under luaRoot/npcs/ and the Lua systems under luaRoot/systems/, and runs their init; every error,
    // each once, empty if all loaded. A spawn whose "npc" has no definition is one too.
    std::vector<std::string> load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines = {});
    // Graceful shutdown, before the despawns: onLeave for every character, then every system's shutdown, then the
    // client events they queued, in every context. Undelivered bus events are dropped.
    void shutdown_scripts();
    // Per system: ok only if ok in every context; the first context's error.
    std::vector<lua::SystemStatus> script_status() const;
    // The Lua systems' validateCharacter verdict on C++-checked options (lua::Runtime::validate_character), asked in
    // START_MAP's context.
    bool validate_character(const std::string& name, const proto::CharOptions& options) {
        return context(rules_.startMap).validate_character(name, options);
    }
    // A map context's event bus, state machines (its characters' and NPCs'; game machines mirrored) and registry, for
    // server C++ and tests: START_MAP's, or map's (created on first use).
    EventBus& bus() { return bus(rules_.startMap); }
    EventBus& bus(uint16_t map) { return context(map).bus(); }
    lua::StateMachines& fsm() { return fsm(rules_.startMap); }
    lua::StateMachines& fsm(uint16_t map) { return context(map).fsm(); }
    entt::registry& registry() { return registry(rules_.startMap); }
    entt::registry& registry(uint16_t map) { return context(map).registry(); }
    // The registry of the context e is in, or null if e is not in the world (tests).
    entt::registry* registry_of(entt::entity e) {
        MapContext* ctx = find(e);
        return ctx ? &ctx->registry() : nullptr;
    }
    // The realm context's bus (its fsm.changed events, posts to "realm") and state machines (the game machines).
    EventBus& realm_bus() { return realm_.bus(); }
    lua::StateMachines& realm_fsm() { return realm_.fsm(); }

    // Places the character (at its saved map and position, lifted onto the ground if it is under it, or START_MAP's
    // spawn if that map is not loaded or the position is off it) in that map's context, sends
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
    // respawnSec after this (never for -1), asleep or not, on the map of its spawn file.
    void destroy_npc(entt::entity e);
    // Graceful shutdown, after the players are gone: removes every NPC; nothing to send, nothing to save.
    void remove_npcs();
    // pos on a declared tile of map (which must be loaded), standing on its ground (within MIN_Y/MAX_Y).
    proto::Vec3 clamp_to_map(uint16_t map, proto::Vec3 pos) const { return world::clamp_to_map(maps_, rules_.move, map, pos); }
    // Every map change goes through here (player or NPC), and teleports within a map too; a context's own requests
    // (portals, kast.transfer onto another map) come here at the serial points. Refused (false, logged) if this
    // process does not own the destination tile. Otherwise moves e at once onto the ground at the destination and
    // takes it out of every view: the player gets MapChange, then a Despawn per old neighbour, who get its Despawn,
    // and its area is updated (area events). Onto another map, in this order: onMapLeave in the old context, the
    // MapChange, Despawns and area.leave there, every component moved to the new context, area.enter and onMapEnter
    // there. The next aoi pass sends the Spawns for the new position: this tick's for a portal (World::tick moves
    // it before the aoi pass), the next tick's for kast.transfer.
    bool transfer(entt::entity e, const Destination& to);

    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, proto::Say say);
    // The chat service's mutes (via the chat link): chat_system drops a muted character's says, yells and emotes.
    Mutes& mutes() { return mutes_; }
    // The map e is on (MapRef); e must be in the world.
    uint16_t map_of(entt::entity e) const;
    // A ClientEvent GameServer already checked (valid name, JSON object within kClientEventJsonMax).
    void push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId = 0);
    // An Interact; checked by interact_system on the next tick.
    void push_interact(entt::entity e, uint32_t objectId);

    void tick(TimePoint now);
    size_t population() const;

private:
    // map's context, created (and its scripts loaded, once load_scripts has run) on first use.
    MapContext& context(uint16_t map);
    MapContext& context(uint16_t map) const { return const_cast<World*>(this)->context(map); }
    // The context e is in, or null.
    // ponytail: a scan over the contexts (one per map); an id -> context index once instances (phase 4) multiply them.
    MapContext* find(entt::entity e) const;
    bool move_to(entt::entity e, const Destination& to); // transfer, without the serial point after it
    // The serial point: what the contexts (and the realm) queued since the last one, in map order: transfers,
    // creates, respawns, game machine changes (applied in the realm, each distinct one once), the realm's client
    // events and the broadcasts for the other contexts' players; then the game machine mirrors are refreshed. Posts
    // are collected and, at the end of a tick only, published on their targets' buses, so they are delivered on the
    // next tick.
    void serial(bool endOfTick);
    void deliver(const nlohmann::json& from, const lua::Post& post);

    WorldRules rules_;
    const Maps& maps_;
    Send send_;
    SendMany sendMany_;
    Save save_;
    lua::Defs defs_;
    Shared shared_;
    std::map<uint16_t, std::unique_ptr<MapContext>> contexts_; // by map id; after shared_ (they hold a reference)
    entt::registry realmReg_; // empty: the realm has no entities
    lua::Runtime realm_;      // fsms/ and the game machines only
    EventBus::Subscription realmChanged_; // the game machines' fsm.changed, forwarded to every map context's bus
    std::vector<lua::Post> realmPosts_;   // kast.world.post from the realm's Lua, since the last serial point
    std::vector<std::pair<nlohmann::json, lua::Post>> posts_; // (from, post), until the end of the tick
    Mutes mutes_;
    std::optional<std::filesystem::path> luaRoot_; // set by load_scripts: contexts created later load from it
    std::vector<std::string> gameMachines_;
    TimePoint now_{}; // the current tick's time
};

} // namespace kast::world
