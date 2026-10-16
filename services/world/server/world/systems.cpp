#include "systems.h"
#include "../core/log.h"
#include "components.h"

#include <algorithm>

namespace kast::world {
namespace {
entt::entity from_wire(uint32_t id) { return entt::entity{id}; }

// The players that currently see e (see audience).
std::vector<entt::entity> viewers_of(const entt::registry& reg, entt::entity e) {
    std::vector<entt::entity> out;
    if (const auto* viewers = reg.try_get<Viewers>(e)) {
        for (uint32_t id : viewers->ids) out.push_back(from_wire(id));
        return out;
    }
    for (uint32_t id : reg.get<Visible>(e).ids)
        if (reg.all_of<SessionLink>(from_wire(id))) out.push_back(from_wire(id));
    return out;
}
} // namespace

std::vector<uint32_t> audience(const entt::registry& reg, entt::entity e) {
    std::vector<uint32_t> sessions;
    for (entt::entity viewer : viewers_of(reg, e)) sessions.push_back(reg.get<SessionLink>(viewer).sessionId);
    return sessions;
}

proto::Spawn spawn_message(const entt::registry& reg, entt::entity e) {
    const auto& tf = reg.get<Transform>(e);
    if (const auto* npc = reg.try_get<Npc>(e))
        return {wire_id(e), npc->name, tf.pos, tf.heading, proto::SpawnKind::npc, npc->templateName};
    return {wire_id(e), reg.get<CharacterRef>(e).name, tf.pos, tf.heading, proto::SpawnKind::player, ""};
}

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

void aoi_rebucket(entt::registry& reg, AoiGrid& grid) {
    for (auto [e, tf, cell] : reg.view<Moved, Transform, AoiCell>().each()) {
        const CellKey c = grid.cell_of(tf.pos.x, tf.pos.z);
        if (c == cell.cell) continue;
        grid.remove(wire_id(e), cell.cell);
        grid.insert(wire_id(e), c);
        cell.cell = c;
    }
}

// Re-buckets movers, then diffs every player's view: Spawn for entities that came into range, Despawn for those
// that left. Range is symmetric, so each player only handles its own view; an NPC's Viewers follow the players'.
void aoi_system(entt::registry& reg, AoiGrid& grid, const Send& send) {
    aoi_rebucket(reg, grid);
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
            if (auto* viewers = reg.try_get<Viewers>(other)) insert_sorted(viewers->ids, wire_id(e));
            send(link.sessionId, spawn_message(reg, other));
        }
        for (uint32_t id : left) {
            if (auto* viewers = reg.try_get<Viewers>(from_wire(id))) erase_sorted(viewers->ids, wire_id(e));
            send(link.sessionId, proto::Despawn{id});
        }
        vis.ids = std::move(now);
        vis.entered = entered;
    }
}

// Accepted player moves and script NPC moves go to every player that sees the mover; never back to the mover.
void replication_system(entt::registry& reg, const Send& send) {
    for (auto [e, tf] : reg.view<Moved, Transform>().each()) {
        const proto::EntityMove msg{wire_id(e), tf.pos, tf.heading};
        for (uint32_t sid : audience(reg, e)) send(sid, msg);
    }
}

// "say": sanitized, rate limited, delivered to the sender and everyone in range.
void chat_system(entt::registry& reg, const ChatRules& rules, const Send& send, TimePoint now) {
    for (auto [e, pending, limit, ch, link] : reg.view<PendingSay, ChatLimit, CharacterRef, SessionLink>().each()) {
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
            for (uint32_t sid : audience(reg, e)) send(sid, msg);
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

    const auto toViewers = [&](entt::entity e, const proto::Message& msg) {
        for (entt::entity viewer : viewers_of(reg, e)) {
            const auto& entered = reg.get<Visible>(viewer).entered;
            if (std::binary_search(entered.begin(), entered.end(), wire_id(e))) continue; // gets the full set below
            send(reg.get<SessionLink>(viewer).sessionId, msg);
        }
    };
    // An NPC has no owner: its owner components reach nobody, its aoi ones only the players that see it.
    const auto toOwner = [&](const SessionLink* link, const proto::Message& msg) {
        if (link) send(link->sessionId, msg);
    };
    for (auto [e, rep] : reg.view<ReplicatedState>().each()) {
        const auto* link = reg.try_get<SessionLink>(e);
        for (const auto& [def, pool] : types) {
            const auto sent = rep.sent.find(def->name);
            const bool aoi = def->replicate == lua::Replicate::aoi;
            if (!pool->contains(e)) {
                if (sent == rep.sent.end()) continue;
                rep.sent.erase(sent);
                rep.oversize.erase(def->name);
                const proto::ComponentRemove msg{wire_id(e), def->name};
                toOwner(link, msg);
                if (aoi) toViewers(e, msg);
                continue;
            }
            const auto& value = pool->get(e).value;
            if (sent != rep.sent.end() && sent->second == value) continue;
            rep.sent[def->name] = value;
            std::string json = value.dump();
            if (!proto::ComponentUpdate::fits(def->name.size(), json.size())) {
                if (rep.oversize.insert(def->name).second)
                    log::error("component too large to replicate; not sent until it fits",
                               {{link ? "char" : "npc", link ? nlohmann::json(reg.get<CharacterRef>(e).charId)
                                                              : nlohmann::json(reg.get<Npc>(e).name)},
                                {"component", def->name},
                                {"bytes", json.size()}});
                continue;
            }
            rep.oversize.erase(def->name);
            const proto::ComponentUpdate msg{wire_id(e), def->name, std::move(json)};
            toOwner(link, msg);
            if (aoi) toViewers(e, msg);
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
        if (!reg.valid(e) || !reg.any_of<Visible, Viewers>(e)) continue; // left already: nothing to send
        if (const auto* link = reg.try_get<SessionLink>(e)) send(link->sessionId, msg); // an NPC has no client
        if (event.to == lua::ClientEvent::To::nearby)
            for (uint32_t sid : audience(reg, e)) send(sid, msg);
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
