#include "world.h"
#include "components.h"
#include "../core/log.h"

#include <algorithm>

namespace kast::world {

World::World(WorldRules rules, Send send, Save save, lua::Defs defs)
    : rules_(rules), send_(std::move(send)), save_(std::move(save)), defs_(defs), grid_(rules.cellSize, rules.viewRadius),
      lua_(reg_, std::move(defs),
           {.create = [this](std::string name, std::string templateName, std::optional<proto::Vec3> pos,
                             float heading) { return create_npc(std::move(name), std::move(templateName), pos, heading); },
            .destroy = [this](entt::entity e) { destroy_npc(e); },
            .clamp = [this](proto::Vec3 pos) { return clamp_to_zone(pos); },
            .max = rules.maxNpcs}) {}

std::vector<std::string> World::load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines) {
    return lua_.load(luaRoot, std::move(gameMachines));
}

void World::shutdown_scripts() {
    const auto view = reg_.view<CharacterRef>();
    for (const std::vector<entt::entity> chars(view.begin(), view.end()); entt::entity e : chars) {
        lua_.leave(e);
        reg_.emplace<ScriptsLeft>(e);
    }
    lua_.shutdown();
    client_event_system(reg_, lua_.take_client_events(), send_); // before the despawns
}

entt::entity World::spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now) {
    proto::Vec3 pos = row.pos;
    const auto& m = rules_.move;
    if (pos.x < 0 || pos.x > m.sizeX || pos.z < 0 || pos.z > m.sizeZ) pos = rules_.spawn; // zone shrank since save
    pos.y = std::clamp(pos.y, m.minY, m.maxY);

    const auto e = reg_.create();
    reg_.emplace<SessionLink>(e, sessionId);
    reg_.emplace<CharacterRef>(e, row.id, row.name);
    reg_.emplace<Transform>(e, pos, row.heading);
    reg_.emplace<MoveState>(e, MoveBudget{0, now});
    reg_.emplace<PendingMoves>(e);
    reg_.emplace<PendingSay>(e);
    reg_.emplace<ChatLimit>(e, WindowLimiter(rules_.chatRateCount, rules_.chatRateWindow));
    const CellKey cell = grid_.cell_of(pos.x, pos.z);
    reg_.emplace<AoiCell>(e, cell);
    reg_.emplace<Visible>(e);
    reg_.emplace<SaveTimer>(e, now);
    reg_.emplace<ReplicatedState>(e);
    grid_.insert(wire_id(e), cell);
    ++population_;
    send_(sessionId, proto::EnterWorldOk{wire_id(e), pos, row.heading});
    lua::restore_components(reg_, e, defs_, row.components, [&](const std::string& fix) {
        log::warn("persisted component fixed on load", {{"char", row.id}, {"fix", fix}});
    });
    lua_.enter(e);
    return e;
}

void World::despawn(entt::entity e) {
    if (!reg_.valid(e)) return;
    if (!reg_.all_of<ScriptsLeft>(e)) {
        lua_.leave(e);
        client_event_system(reg_, lua_.take_client_events(), send_); // before the Despawn, while e can still be seen
    }
    const auto& ch = reg_.get<CharacterRef>(e);
    const auto& tf = reg_.get<Transform>(e);
    save_({ch.charId, tf.pos, tf.heading, lua::components_column(reg_, e, defs_)});
    const uint32_t id = wire_id(e);
    for (uint32_t other : reg_.get<Visible>(e).ids) {
        const auto o = entt::entity{other};
        if (auto* viewers = reg_.try_get<Viewers>(o)) { // an NPC it saw: it no longer has this viewer
            erase_sorted(viewers->ids, id);
            continue;
        }
        erase_sorted(reg_.get<Visible>(o).ids, id);
        send_(reg_.get<SessionLink>(o).sessionId, proto::Despawn{id});
    }
    grid_.remove(id, reg_.get<AoiCell>(e).cell);
    reg_.destroy(e);
    --population_;
}

proto::Vec3 World::clamp_to_zone(proto::Vec3 pos) const {
    const auto& m = rules_.move;
    return {std::clamp(pos.x, 0.0f, m.sizeX), std::clamp(pos.y, m.minY, m.maxY), std::clamp(pos.z, 0.0f, m.sizeZ)};
}

entt::entity World::create_npc(std::string name, std::string templateName, std::optional<proto::Vec3> pos,
                               float heading) {
    const proto::Vec3 at = clamp_to_zone(pos.value_or(rules_.spawn));
    const auto e = reg_.create();
    reg_.emplace<Npc>(e, std::move(name), std::move(templateName));
    reg_.emplace<Transform>(e, at, heading);
    const CellKey cell = grid_.cell_of(at.x, at.z);
    reg_.emplace<AoiCell>(e, cell);
    reg_.emplace<Viewers>(e);
    reg_.emplace<ReplicatedState>(e);
    grid_.insert(wire_id(e), cell);
    return e;
}

void World::destroy_npc(entt::entity e) {
    const uint32_t id = wire_id(e);
    for (uint32_t viewer : reg_.get<Viewers>(e).ids) {
        auto& vis = reg_.get<Visible>(entt::entity{viewer});
        erase_sorted(vis.ids, id);
        erase_sorted(vis.entered, id); // destroyed in the tick it came into view: no component full set either
        send_(reg_.get<SessionLink>(entt::entity{viewer}).sessionId, proto::Despawn{id});
    }
    grid_.remove(id, reg_.get<AoiCell>(e).cell);
    reg_.destroy(e);
}

void World::remove_npcs() {
    const auto view = reg_.view<Npc>();
    for (const std::vector<entt::entity> npcs(view.begin(), view.end()); entt::entity e : npcs) destroy_npc(e);
}

void World::push_move(entt::entity e, const proto::Move& m) {
    if (reg_.valid(e)) reg_.get<PendingMoves>(e).moves.push_back(m);
}

void World::push_say(entt::entity e, std::string text) {
    if (reg_.valid(e)) reg_.get<PendingSay>(e).texts.push_back(std::move(text));
}

// Lua runs after movement and chat (it sees this tick's positions) and before replication and save (its changes go
// out and get saved this tick). aoi stays before both replications, so a Spawn always precedes component data; the
// NPCs Lua creates or moves out of a view show up in the next tick's aoi diff. The events Lua published are delivered
// right after update; client events go out after component replication, so a client already has the Spawn and the
// latest component values they may refer to.
void World::tick(TimePoint now) {
    movement_system(reg_, rules_.move, send_, now);
    aoi_system(reg_, grid_, send_);
    chat_system(reg_, rules_.chat, send_, now);
    lua_.update(rules_.tickSeconds);
    lua_.drain_events();
    aoi_rebucket(reg_, grid_); // NPCs Lua moved: in their new cell for the next tick's view diff
    replication_system(reg_, send_);
    component_replication_system(reg_, defs_, send_);
    client_event_system(reg_, lua_.take_client_events(), send_);
    save_system(reg_, defs_, save_, rules_.saveInterval, now);
    end_of_tick_system(reg_);
}

} // namespace kast::world
