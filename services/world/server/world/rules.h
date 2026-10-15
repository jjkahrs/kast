// Pure validation rules applied to untrusted client input: character names, chat text, movement.
#pragma once
#include "../core/clock.h"

#include <messages.h>

#include <optional>
#include <string>
#include <string_view>

namespace kast {

// 3-16 ASCII letters. Uniqueness (case-insensitive) is enforced by the database.
bool valid_character_name(std::string_view name);

// Valid UTF-8, control characters stripped, 1..maxBytes bytes after stripping. nullopt = reject.
std::optional<std::string> sanitize_chat(std::string_view text, size_t maxBytes);

struct MoveRules {
    float maxSpeed = 10;       // m/s
    float tolerance = 1;       // m/s on top of maxSpeed
    float burstSeconds = 1;    // distance budget cap, in seconds of travel; absorbs network jitter
    float sizeX = 2048, sizeZ = 2048;
    float minY = -100, maxY = 500;
};

// Distance budget: refills at (maxSpeed + tolerance) per second of server time, capped at burstSeconds of travel.
// Sustained speed can never exceed maxSpeed + tolerance, however the client times its packets.
struct MoveBudget {
    float meters = 0;
    TimePoint lastRefill{};
};

enum class MoveVerdict { accepted, out_of_bounds, too_fast };

// On accepted, `to.y` is clamped and the budget is spent. Speed is measured on x/z only.
MoveVerdict validate_move(const MoveRules& rules, MoveBudget& budget, const proto::Vec3& from, proto::Vec3& to,
                          TimePoint now);

} // namespace kast
