// Systems: free functions over the registry, run once per tick in the order World::tick lists them.
#pragma once
#include "../core/clock.h"
#include "aoi_grid.h"
#include "maps.h"
#include "rules.h"

#include <db/char_repo.h>
#include <lua/components.h>
#include <lua/runtime.h>
#include <messages.h>

#include <entt/entity/registry.hpp>

#include <functional>
#include <span>

namespace kast::world {

using Send = std::function<void(uint32_t sessionId, const proto::Message&)>;
// One message to several sessions, each getting it where a Send would have put it: encoded once, not per session.
using SendMany = std::function<void(std::span<const uint32_t> sessionIds, const proto::Message&)>;
using Save = std::function<void(const db::CharSave&)>;

inline uint32_t wire_id(entt::entity e) { return entt::to_integral(e); }

struct ChatRules {
    size_t maxBytes = 256;
    float yellRadius = 300; // CHAT_YELL_RADIUS: m (x/z) on the yeller's map; may exceed the AOI view radius
};

// The session id of every player that currently sees e: for a player, the players in its Visible (range is
// symmetric; the NPCs there see nothing), for an NPC, its Viewers.
std::vector<uint32_t> audience(const entt::registry& reg, entt::entity e);
// Spawn for entity e as a viewer gets it: a player's character name, or an NPC's name and template.
proto::Spawn spawn_message(const entt::registry& reg, entt::entity e);

// A spawn file entry (map::Spawn) and when its NPC is due: alive while the NPC exists, else at respawnAt.
struct SpawnRecord {
    uint16_t map = 0;
    const map::Spawn* spawn = nullptr; // in Maps, which outlives the World
    TimePoint respawnAt{};             // epoch: due at the first tick
    bool alive = false;
};
struct Spawner {
    std::vector<SpawnRecord> records;
    bool capped = false; // MAX_NPCS hit: warned once until spawning works again
};

// Creates the NPC of every due record (create, then SpawnRef{record}). At maxNpcs NPCs it creates nothing more this
// tick and tries again the next.
// ponytail: always-on spawns; activate per tile near players if NPC counts grow.
void spawner_system(entt::registry& reg, Spawner& spawner, size_t maxNpcs, TimePoint now,
                    const std::function<entt::entity(const SpawnRecord&)>& create);
// A move off the player's map (onto an undeclared tile) is rejected like a speedhack.
void movement_system(entt::registry& reg, const MoveRules& rules, const Maps& maps, const Send& send, TimePoint now);
// Updates player e's AreaRef from its map and position. On a change it publishes "area.leave" for the old area, then
// "area.enter" for the new one ({entity, map, area, name, flags}; none for "no area"). Returns the area it is in now.
const map::Area* set_area(entt::registry& reg, const Maps& maps, EventBus& bus, entt::entity e);
// set_area for every player that moved this tick; returns those now in a portal area, with the portal, for the
// caller to transfer.
std::vector<std::pair<entt::entity, map::Portal>> area_system(entt::registry& reg, const Maps& maps, EventBus& bus);
// Moves every entity that moved this tick (Moved) to its new grid cell and touches its cells (old and new, or the one
// it moved within). aoi_system runs it first; World runs it again after Lua, whose NPC moves would otherwise miss the
// next tick's view diff: the cells touched then stay touched until that pass.
void aoi_rebucket(entt::registry& reg, AoiGrid& grid);
// Diffs the view of each player near a touched grid cell (see systems.cpp); the others keep theirs, entered empty.
void aoi_system(entt::registry& reg, AoiGrid& grid, const Send& send);
void replication_system(entt::registry& reg, const Send& send);
// Each player's Says: rate limited (ChatLimit), sanitized, dropped with ChatNotice{muted} while the character is muted
// (wallNow against mutes), then ChatMsg{kind} to the sender and: say / emote the players that see it, yell every player
// on its map within rules.yellRadius.
void chat_system(entt::registry& reg, const ChatRules& rules, const Mutes& mutes, const Send& send,
                 const SendMany& sendMany, TimePoint now, SysTime wallNow);
// Publishes each character's ClientEvents (kast.world.send) on the Lua bus as "client.<name>" {entity, data}, in
// arrival order, plus requestId for a kast.world.request; past the character's ClientEventLimit they are dropped and
// counted (a dropped request is never answered: the client times it out).
void client_event_in_system(entt::registry& reg, EventBus& bus, TimePoint now);
// Each player's Interacts (docs/services/world/events.html#interactables): one on the player's map (Maps::interactable)
// with the player within its range of the shape's edge (map::footprint_distance, horizontal) publishes the object's
// event {entity, map, object, data (absent if the file has none)}. Anything else is dropped with a debug log.
void interact_system(entt::registry& reg, const Maps& maps, EventBus& bus);
// Sends replicated Lua components (docs/services/world/lua.html): what changed since the last send to the
// owner (owner, aoi) and observers (aoi), and every aoi component of an entity to a viewer it came into view of.
void component_replication_system(entt::registry& reg, const lua::Defs& defs, const Send& send,
                                  const SendMany& sendMany);
// Sends the client events Lua queued (kast.sendTo/sendNearby/broadcast) as ServerEvent, in call order. The audience is
// taken now: owner = the character's session, nearby = it and every session that sees it, all = every player. A
// target no longer in the world is skipped, and an NPC target has no client of its own. A kast.reply goes as Reply to
// its entity's session alone.
void client_event_system(entt::registry& reg, std::vector<lua::ClientEvent> events, const Send& send,
                         const SendMany& sendMany);
void save_system(entt::registry& reg, const lua::Defs& defs, const Save& save, Millis interval, TimePoint now);
void end_of_tick_system(entt::registry& reg);

} // namespace kast::world
