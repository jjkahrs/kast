// Systems: free functions over the registry, run once per tick in the order World::tick lists them.
#pragma once
#include "../core/clock.h"
#include "aoi_grid.h"
#include "rules.h"

#include <db/char_repo.h>
#include <lua/components.h>
#include <lua/runtime.h>
#include <messages.h>

#include <entt/entity/registry.hpp>

#include <functional>

namespace kast::world {

using Send = std::function<void(uint32_t sessionId, const proto::Message&)>;
using Save = std::function<void(const db::CharSave&)>;

inline uint32_t wire_id(entt::entity e) { return entt::to_integral(e); }

struct ChatRules {
    size_t maxBytes = 256;
};

// The session id of every player that currently sees e: for a player, the players in its Visible (range is
// symmetric; the NPCs there see nothing), for an NPC, its Viewers.
std::vector<uint32_t> audience(const entt::registry& reg, entt::entity e);
// Spawn for entity e as a viewer gets it: a player's character name, or an NPC's name and template.
proto::Spawn spawn_message(const entt::registry& reg, entt::entity e);

void movement_system(entt::registry& reg, const MoveRules& rules, const Send& send, TimePoint now);
// Moves every entity that moved this tick (Moved) to its new grid cell. aoi_system runs it first; World runs it
// again after Lua, whose NPC moves would otherwise miss the next tick's view diff.
void aoi_rebucket(entt::registry& reg, AoiGrid& grid);
void aoi_system(entt::registry& reg, AoiGrid& grid, const Send& send);
void replication_system(entt::registry& reg, const Send& send);
void chat_system(entt::registry& reg, const ChatRules& rules, const Send& send, TimePoint now);
// Sends replicated Lua components (README "Lua components and systems"): what changed since the last send to the
// owner (owner, aoi) and observers (aoi), and every aoi component of an entity to a viewer it came into view of.
void component_replication_system(entt::registry& reg, const lua::Defs& defs, const Send& send);
// Sends the client events Lua queued (kast.sendTo/sendNearby/broadcast) as ServerEvent, in call order. The audience is
// taken now: owner = the character's session, nearby = it and every session that sees it, all = every player. A
// target no longer in the world is skipped, and an NPC target has no client of its own.
void client_event_system(entt::registry& reg, std::vector<lua::ClientEvent> events, const Send& send);
void save_system(entt::registry& reg, const lua::Defs& defs, const Save& save, Millis interval, TimePoint now);
void end_of_tick_system(entt::registry& reg);

} // namespace kast::world
