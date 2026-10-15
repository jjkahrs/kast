#include "systems.h"
#include "../core/log.h"
#include "components.h"

namespace kast::world {
namespace {
entt::entity from_wire(uint32_t id) { return entt::entity{id}; }
} // namespace

// Validates each queued Move against speed and bounds; a rejected move snaps the client back with Correction.
void movement_system(entt::registry& reg, const MoveRules& rules, const Send& send, TimePoint now) {
    for (auto [e, pending, tf, ms, link] : reg.view<PendingMoves, Transform, MoveState, SessionLink>().each()) {
        if (pending.moves.empty()) continue;
        bool corrected = false;
        for (const auto& m : pending.moves) {
            proto::Vec3 to = m.pos;
            if (validate_move(rules, ms.budget, tf.pos, to, now) == MoveVerdict::accepted) {
                tf.pos = to;
                tf.heading = m.heading;
                reg.emplace_or_replace<Moved>(e);
                reg.emplace_or_replace<Dirty>(e);
            } else {
                corrected = true;
            }
        }
        if (corrected) send(link.sessionId, proto::Correction{tf.pos});
        pending.moves.clear();
    }
}

// Re-buckets movers, then diffs every player's view: Spawn for entities that came into range, Despawn for those
// that left. Range is symmetric, so each player only handles its own view.
void aoi_system(entt::registry& reg, AoiGrid& grid, const Send& send) {
    for (auto [e, tf, cell] : reg.view<Moved, Transform, AoiCell>().each()) {
        const CellKey c = grid.cell_of(tf.pos.x, tf.pos.z);
        if (c == cell.cell) continue;
        grid.remove(wire_id(e), cell.cell);
        grid.insert(wire_id(e), c);
        cell.cell = c;
    }
    auto posOf = [&](uint32_t id, float& x, float& z) {
        const auto& t = reg.get<Transform>(from_wire(id));
        x = t.pos.x;
        z = t.pos.z;
    };
    std::vector<uint32_t> entered, left;
    for (auto [e, tf, vis, link] : reg.view<Transform, Visible, SessionLink>().each()) {
        auto now = grid.query(tf.pos.x, tf.pos.z, wire_id(e), posOf);
        diff_sorted(vis.ids, now, entered, left);
        for (uint32_t id : entered) {
            const auto other = from_wire(id);
            const auto& ot = reg.get<Transform>(other);
            send(link.sessionId, proto::Spawn{id, reg.get<CharacterRef>(other).name, ot.pos, ot.heading});
        }
        for (uint32_t id : left) send(link.sessionId, proto::Despawn{id});
        vis.ids = std::move(now);
    }
}

// Accepted moves go to every observer in range; never back to the mover.
void replication_system(entt::registry& reg, const Send& send) {
    for (auto [e, tf, vis] : reg.view<Moved, Transform, Visible>().each()) {
        const proto::EntityMove msg{wire_id(e), tf.pos, tf.heading};
        for (uint32_t id : vis.ids) send(reg.get<SessionLink>(from_wire(id)).sessionId, msg);
    }
}

// "say": sanitized, rate limited, delivered to the sender and everyone in range.
void chat_system(entt::registry& reg, const ChatRules& rules, const Send& send, TimePoint now) {
    for (auto [e, pending, limit, ch, link, vis] :
         reg.view<PendingSay, ChatLimit, CharacterRef, SessionLink, Visible>().each()) {
        for (const auto& text : pending.texts) {
            if (!limit.limiter.allow(now)) {
                log::debug("chat rate limited", {{"char", ch.charId}});
                continue;
            }
            auto clean = sanitize_chat(text, rules.maxBytes);
            if (!clean) {
                log::debug("chat rejected", {{"char", ch.charId}, {"bytes", text.size()}});
                continue;
            }
            const proto::ChatMsg msg{ch.name, std::move(*clean)};
            send(link.sessionId, msg);
            for (uint32_t id : vis.ids) send(reg.get<SessionLink>(from_wire(id)).sessionId, msg);
        }
        pending.texts.clear();
    }
}

// Periodic save of characters whose position changed.
void save_system(entt::registry& reg, const Save& save, Millis interval, TimePoint now) {
    std::vector<entt::entity> saved;
    for (auto [e, timer, ch, tf] : reg.view<Dirty, SaveTimer, CharacterRef, Transform>().each()) {
        if (now - timer.lastSave < interval) continue;
        save({ch.charId, tf.pos, tf.heading});
        timer.lastSave = now;
        saved.push_back(e);
    }
    reg.remove<Dirty>(saved.begin(), saved.end());
}

void end_of_tick_system(entt::registry& reg) { reg.clear<Moved>(); }

} // namespace kast::world
