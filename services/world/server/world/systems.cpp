#include "systems.h"
#include "../core/log.h"
#include "components.h"

#include <algorithm>

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
        vis.entered = entered;
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

// Runs after the Lua systems, so a change made this tick is sent this tick. Spawn always goes first: aoi_system sent
// it earlier in the tick, and a viewer that just got it receives the full set instead of this tick's changes.
// ponytail: diffs each replicated component's full JSON every tick; dirty flags from kast.set if entity counts grow.
void component_replication_system(entt::registry& reg, const lua::Defs& defs, const Send& send) {
    struct Replicated {
        const lua::ComponentDef* def;
        entt::storage_for_t<lua::ComponentData>* pool;
    };
    std::vector<Replicated> types;
    for (const auto& def : defs)
        if (def->replicate != lua::Replicate::none) types.push_back({def.get(), &lua::pool(reg, def->name)});
    if (types.empty()) return;

    const auto toViewers = [&](entt::entity e, const Visible& vis, const proto::Message& msg) {
        for (uint32_t id : vis.ids) {
            const auto viewer = from_wire(id);
            const auto& entered = reg.get<Visible>(viewer).entered;
            if (std::binary_search(entered.begin(), entered.end(), wire_id(e))) continue; // gets the full set below
            send(reg.get<SessionLink>(viewer).sessionId, msg);
        }
    };
    for (auto [e, rep, vis, link, ch] : reg.view<ReplicatedState, Visible, SessionLink, CharacterRef>().each()) {
        for (const auto& [def, pool] : types) {
            const auto sent = rep.sent.find(def->name);
            const bool aoi = def->replicate == lua::Replicate::aoi;
            if (!pool->contains(e)) {
                if (sent == rep.sent.end()) continue;
                rep.sent.erase(sent);
                rep.oversize.erase(def->name);
                const proto::ComponentRemove msg{wire_id(e), def->name};
                send(link.sessionId, msg);
                if (aoi) toViewers(e, vis, msg);
                continue;
            }
            const auto& value = pool->get(e).value;
            if (sent != rep.sent.end() && sent->second == value) continue;
            rep.sent[def->name] = value;
            std::string json = value.dump();
            if (!proto::ComponentUpdate::fits(def->name.size(), json.size())) {
                if (rep.oversize.insert(def->name).second)
                    log::error("component too large to replicate; not sent until it fits",
                               {{"char", ch.charId}, {"component", def->name}, {"bytes", json.size()}});
                continue;
            }
            rep.oversize.erase(def->name);
            const proto::ComponentUpdate msg{wire_id(e), def->name, std::move(json)};
            send(link.sessionId, msg);
            if (aoi) toViewers(e, vis, msg);
        }
    }
    // Viewers that saw an entity come into view this tick (after its Spawn): every aoi component it has, as sent.
    for (auto [viewer, vis, link] : reg.view<Visible, SessionLink>().each()) {
        for (uint32_t id : vis.entered) {
            const auto other = from_wire(id);
            const auto* rep = reg.try_get<ReplicatedState>(other);
            if (!rep) continue;
            for (const auto& [def, pool] : types) {
                if (def->replicate != lua::Replicate::aoi || rep->oversize.contains(def->name)) continue;
                const auto sent = rep->sent.find(def->name);
                if (sent != rep->sent.end()) send(link.sessionId, proto::ComponentUpdate{id, def->name, sent->second.dump()});
            }
        }
    }
}

void client_event_system(entt::registry& reg, std::vector<lua::ClientEvent> events, const Send& send) {
    for (auto& event : events) {
        const proto::ServerEvent msg{std::move(event.name), std::move(event.json)};
        if (event.to == lua::ClientEvent::To::all) {
            for (auto [e, link] : reg.view<SessionLink>().each()) send(link.sessionId, msg);
            continue;
        }
        const auto e = from_wire(event.entity);
        if (!reg.valid(e) || !reg.all_of<SessionLink, Visible>(e)) continue; // left already: nothing to send
        send(reg.get<SessionLink>(e).sessionId, msg);
        if (event.to == lua::ClientEvent::To::nearby)
            for (uint32_t id : reg.get<Visible>(e).ids) send(reg.get<SessionLink>(from_wire(id)).sessionId, msg);
    }
}

// Periodic save of characters whose position or persisted Lua components changed.
void save_system(entt::registry& reg, const lua::Defs& defs, const Save& save, Millis interval, TimePoint now) {
    std::vector<entt::entity> saved;
    for (auto [e, timer, ch, tf] : reg.view<Dirty, SaveTimer, CharacterRef, Transform>().each()) {
        if (now - timer.lastSave < interval) continue;
        save({ch.charId, tf.pos, tf.heading, lua::components_column(reg, e, defs)});
        timer.lastSave = now;
        saved.push_back(e);
    }
    reg.remove<Dirty>(saved.begin(), saved.end());
}

void end_of_tick_system(entt::registry& reg) { reg.clear<Moved>(); }

} // namespace kast::world
