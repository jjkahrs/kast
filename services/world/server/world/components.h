// ECS components: plain data, no behaviour. Systems (systems.h) read and write them.
#pragma once
#include "../core/rate_limiter.h"
#include "aoi_grid.h"
#include "rules.h"

#include <messages.h>
#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace kast::world {

struct SessionLink {
    uint32_t sessionId = 0;
};

struct CharacterRef {
    uint64_t charId = 0;
    std::string name;
    proto::CharOptions options = {}; // as created (CharCreate)
};

struct Transform {
    proto::Vec3 pos;
    float heading = 0;
};

// The map every player and NPC is on (map.json "id"). Changed only by World::transfer.
struct MapRef {
    uint16_t map = 0;
};

// The area a player is in: an index into map's areas (map::area_at), -1 for none. Kept by set_area; map is the map
// the index belongs to, which a transfer changes before set_area runs.
struct AreaRef {
    uint16_t map = 0;
    int16_t area = -1;
};

struct MoveState {
    MoveBudget budget;
};

// Inbound this tick, filled from the network before systems run.
struct PendingMoves {
    std::vector<proto::Move> moves;
};
struct PendingSay {
    std::vector<std::string> texts;
};

struct ChatLimit {
    WindowLimiter limiter;
};

// ClientEvents this tick, already checked by GameServer (name, size, JSON object); client_event_in_system rate limits
// them and publishes them to the Lua bus.
struct PendingClientEvent {
    std::string name;
    nlohmann::json data;
};
struct PendingClientEvents {
    std::vector<PendingClientEvent> events;
};
struct ClientEventLimit {
    WindowLimiter limiter;
    uint64_t dropped = 0; // over the limit since login (logged on despawn)
};

struct MoveLock {}; // tag: kast.setMovable(e, false); every Move is refused with a Correction

struct AoiCell {
    CellKey cell;
};

// Entities this one can currently see, sorted wire ids. entered: those that came into view this tick (sorted).
struct Visible {
    std::vector<uint32_t> ids;
    std::vector<uint32_t> entered;
};

struct Moved {};  // tag: accepted a move this tick
struct Dirty {};  // tag: position changed since the last save

struct SaveTimer {
    TimePoint lastSave{};
};

// The replicated Lua components as last sent to clients, by name (component_replication_system diffs against it).
struct ReplicatedState {
    std::map<std::string, nlohmann::ordered_json, std::less<>> sent;
    std::set<std::string, std::less<>> oversize; // too big for a frame: logged once until it fits again
};

struct ScriptsLeft {}; // tag: onLeave already ran (graceful shutdown runs it for everyone first)

// A server-created NPC (kast.create or a spawn file): no session, never saved. name and templateName go to clients in
// Spawn. npc: the NPC definition it was made from (lua::NpcDef), "" for kast.create with a template.
struct Npc {
    std::string name;
    std::string templateName;
    std::string npc;
};

// An NPC placed by a spawn file: record indexes World's spawn records (SpawnRecord). Read in Lua as "Spawn".
struct SpawnRef {
    uint32_t record = 0;
};

// The players that currently see this NPC, sorted wire ids. An NPC has no Visible: it sees nothing, and AOI is
// tracked from the players' side (aoi_system keeps this in step with their Visible).
struct Viewers {
    std::vector<uint32_t> ids;
};

// Names no Lua component may take: the Lua built-ins (read-only Character, Npc and Spawn, Transform, writable
// StateMachine)
// and every C++ component here, in lua/components.h and in lua/state_machines.h.
inline constexpr const char* kReservedComponentNames[] = {
    "Character",    "Transform",       "StateMachine", "Npc",                 "SessionLink",   "CharacterRef",
    "MoveState",    "PendingMoves",    "PendingSay",   "ChatLimit",           "PendingClientEvents", "ClientEventLimit", "MoveLock",           "AoiCell",       "Visible",
    "Viewers",      "Moved",           "Dirty",        "SaveTimer",           "ReplicatedState", "ScriptsLeft",
    "PreservedComponents", "ComponentData", "StateMachineRuntime", "MapRef",
    "AreaRef",      "Spawn",           "SpawnRef"};

} // namespace kast::world
