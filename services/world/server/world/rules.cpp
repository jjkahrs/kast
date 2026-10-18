#include "rules.h"

#include <algorithm>
#include <cmath>

namespace kast {

bool valid_character_name(std::string_view name) {
    if (name.size() < 3 || name.size() > 16) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
}

std::optional<nlohmann::json> client_event_data(const proto::ClientEvent& ev) {
    if (!proto::valid_client_event_name(ev.name) || ev.json.size() > proto::kClientEventJsonMax) return std::nullopt;
    auto data = nlohmann::json::parse(ev.json, nullptr, false);
    if (!data.is_object()) return std::nullopt; // a parse error is "discarded", not an object
    return data;
}

std::optional<std::string> sanitize_chat(std::string_view text, size_t maxBytes) {
    if (text.size() > maxBytes) return std::nullopt;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (b0 < 0x80) { len = 1; cp = b0; }
        else if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; }
        else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; }
        else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; }
        else return std::nullopt;
        if (i + len > text.size()) return std::nullopt;
        for (size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) return std::nullopt;
            cp = (cp << 6) | (b & 0x3F);
        }
        static constexpr uint32_t kMinForLen[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMinForLen[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt; // overlong/surrogate
        const bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
        if (!control) out.append(text.substr(i, len));
        i += len;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

MoveVerdict validate_move(const MoveRules& rules, MoveBudget& budget, const proto::Vec3& from, proto::Vec3& to,
                          float ground, TimePoint now, bool onMesh) {
    const float speed = rules.maxSpeed + rules.tolerance;
    const float elapsed = static_cast<float>(std::max(0.0, seconds_between(budget.lastRefill, now)));
    budget.meters = std::min(speed * rules.burstSeconds, budget.meters + speed * elapsed);
    budget.lastRefill = now;
    const float dist = std::hypot(to.x - from.x, to.z - from.z);
    if (dist > budget.meters) return MoveVerdict::too_fast;
    if (to.y > ground + rules.maxAir && to.y >= from.y) return MoveVerdict::in_air;
    if (!onMesh) return MoveVerdict::off_mesh;
    budget.meters -= dist;
    if (to.y < ground - rules.groundTolerance) to.y = ground;
    to.y = std::clamp(to.y, rules.minY, rules.maxY);
    return MoveVerdict::accepted;
}

} // namespace kast
