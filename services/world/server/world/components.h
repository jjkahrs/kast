// ECS components: plain data, no behaviour. Systems (systems.h) read and write them.
#pragma once
#include "../core/rate_limiter.h"
#include "aoi_grid.h"
#include "rules.h"

#include <messages.h>

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

// Entities this one can currently see, sorted wire ids.
struct Visible {
    std::vector<uint32_t> ids;
};

struct Moved {};  // tag: accepted a move this tick
struct Dirty {};  // tag: position changed since the last save

struct SaveTimer {
    TimePoint lastSave{};
};

} // namespace kast::world
