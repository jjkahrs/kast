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
};

struct Transform {
    proto::Vec3 pos;
    float heading = 0;
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

// A server-created NPC (kast.create): no session, never saved. name and templateName go to clients in Spawn.
struct Npc {
    std::string name;
    std::string templateName;
};

// The players that currently see this NPC, sorted wire ids. An NPC has no Visible: it sees nothing, and AOI is
// tracked from the players' side (aoi_system keeps this in step with their Visible).
struct Viewers {
    std::vector<uint32_t> ids;
};

// Names no Lua component may take: the Lua built-ins (read-only Character and Npc, Transform, writable StateMachine)
// and every C++ component here, in lua/components.h and in lua/state_machines.h.
inline constexpr const char* kReservedComponentNames[] = {
    "Character",    "Transform",       "StateMachine", "Npc",                 "SessionLink",   "CharacterRef",
    "MoveState",    "PendingMoves",    "PendingSay",   "ChatLimit",           "AoiCell",       "Visible",
    "Viewers",      "Moved",           "Dirty",        "SaveTimer",           "ReplicatedState", "ScriptsLeft",
    "PreservedComponents", "ComponentData", "StateMachineRuntime"};

} // namespace kast::world
