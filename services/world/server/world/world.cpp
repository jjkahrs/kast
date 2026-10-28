#include "world.h"
#include "components.h"
#include "../core/log.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace kast::world {

namespace {
// Marks every replicated component e has, for component_replication_system to send (writers outside kast mark here).
void mark_replicated(entt::registry& reg, entt::entity e, const lua::Defs& defs) {
    for (const auto& def : defs)
        if (def->replicate != lua::Replicate::none && lua::pool(reg, def->name).contains(e)) mark_dirty(reg, e, def->name);
}
} // namespace

World::World(WorldRules rules, const Maps& maps, Send send, SendMany sendMany, Save save, lua::Defs defs)
    : rules_(rules), maps_(maps), send_(std::move(send)), sendMany_(std::move(sendMany)), save_(std::move(save)), defs_(defs),
      grid_(rules.cellSize, rules.viewRadius),
      lua_(reg_, std::move(defs),
           {.create = [this](std::string name, std::string templateName, std::optional<uint16_t> map,
                             std::optional<proto::Vec3> pos, float heading, std::string npc, const lua::Json& components) {
                return create_npc(std::move(name), std::move(templateName), map, pos, heading, std::move(npc), components);
            },
            .destroy = [this](entt::entity e) { destroy_npc(e); },
            .clamp = [this](uint16_t map, proto::Vec3 pos) { return clamp_to_map(map, pos); },
            .map = [this](uint16_t id) { return maps_.by_id(id); },
            .ground = [this](uint16_t map, float x, float z) { return maps_.ground(map, x, z); },
            .path = [this](uint16_t map, proto::Vec3 from, proto::Vec3 to) { return maps_.nav().path(map, from, to); },
            .transfer = [this](entt::entity e, uint16_t map, proto::Vec3 pos,
                               float heading) { return transfer(e, {map, pos, heading}); },
            .spawn = [this](uint32_t record) { return spawner_.records.at(record).spawn; },
            .max = rules.maxNpcs}) {
    activation_.radius = rules.activationTiles;
    for (const auto& [id, info] : maps_.all())
        for (const map::Spawn& s : info.spawns) {
            activation_.unloaded[Activation::key(id, map::tile_of(s.pos[0], s.pos[2]))].push_back(
                uint32_t(spawner_.records.size()));
            spawner_.records.push_back({id, &s});
        }
}

std::vector<std::string> World::load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines) {
    std::vector<std::string> errors = lua_.load(luaRoot, std::move(gameMachines));
    for (const SpawnRecord& r : spawner_.records)
        if (!lua_.npc(r.spawn->npc))
            errors.push_back(std::format("{}: spawn \"{}\": unknown npc \"{}\" (no definition in npcs/)", r.spawn->file,
                                         r.spawn->id, r.spawn->npc));
    return errors;
}

void World::shutdown_scripts() {
    const auto view = reg_.view<CharacterRef>();
    for (const std::vector<entt::entity> chars(view.begin(), view.end()); entt::entity e : chars) {
        lua_.leave(e);
        reg_.emplace<ScriptsLeft>(e);
    }
    lua_.shutdown();
    client_event_system(reg_, lua_.take_client_events(), send_, sendMany_); // before the despawns
}

entt::entity World::spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now) {
    now_ = now;
    uint16_t map = row.mapId == 0 ? rules_.startMap : row.mapId; // 0: saved before maps (migration 003)
    proto::Vec3 pos = row.pos;
    float heading = row.heading;
    if (!maps_.has_tile(map, pos.x, pos.z)) { // map removed or shrunk since the save
        const auto& start = maps_.info(rules_.startMap);
        log::warn("saved position not on a loaded map; using START_MAP spawn",
                  {{"char", row.id}, {"map", map}, {"x", pos.x}, {"z", pos.z}});
        map = start.id;
        pos = {start.spawn[0], start.spawn[1], start.spawn[2]};
        heading = start.spawnHeading;
    }
    if (const float ground = *maps_.ground(map, pos.x, pos.z); pos.y < ground - rules_.move.groundTolerance)
        pos.y = ground; // saved under the ground (heights edited since): onto it
    pos.y = std::clamp(pos.y, rules_.move.minY, rules_.move.maxY);

    const auto e = reg_.create();
    reg_.emplace<SessionLink>(e, sessionId);
    reg_.emplace<CharacterRef>(e, row.id, row.name, row.options);
    reg_.emplace<Transform>(e, pos, heading);
    reg_.emplace<MapRef>(e, map);
    reg_.emplace<MoveState>(e, MoveBudget{0, now});
    reg_.emplace<PendingMoves>(e);
    reg_.emplace<PendingSay>(e);
    reg_.emplace<ChatLimit>(e, WindowLimiter(rules_.chatRateCount, rules_.chatRateWindow));
    reg_.emplace<PendingClientEvents>(e);
    reg_.emplace<ClientEventLimit>(e, WindowLimiter(rules_.clientEventRateCount, rules_.clientEventRateWindow));
    reg_.emplace<PendingInteracts>(e);
    const CellKey cell = grid_.cell_of(map, pos.x, pos.z);
    reg_.emplace<AoiCell>(e, cell);
    reg_.emplace<Visible>(e);
    reg_.emplace<SaveTimer>(e, now);
    reg_.emplace<ReplicatedState>(e);
    grid_.insert(wire_id(e), cell);
    set_area(reg_, maps_, lua_.bus(), e);
    ++population_;
    send_(sessionId, proto::EnterWorldOk{wire_id(e), map, pos, heading});
    activation_enter(reg_, activation_, spawner_, grid_, e);
    spawn_due(now);
    lua::restore_components(reg_, e, defs_, row.components, [&](const std::string& fix) {
        log::warn("persisted component fixed on load", {{"char", row.id}, {"fix", fix}});
    });
    mark_replicated(reg_, e, defs_);
    lua_.enter(e);
    return e;
}

void World::despawn(entt::entity e) {
    if (!reg_.valid(e)) return;
    if (!reg_.all_of<ScriptsLeft>(e)) {
        lua_.leave(e);
        client_event_system(reg_, lua_.take_client_events(), send_, sendMany_); // before the Despawn, while e can still be seen
    }
    const auto& ch = reg_.get<CharacterRef>(e);
    const auto& tf = reg_.get<Transform>(e);
    if (const uint64_t dropped = reg_.get<ClientEventLimit>(e).dropped)
        log::info("client events dropped", {{"char", ch.charId}, {"count", dropped}});
    save_({ch.charId, reg_.get<MapRef>(e).map, tf.pos, tf.heading, lua::components_column(reg_, e, defs_)});
    leave_views(e);
    activation_leave(reg_, activation_, spawner_, grid_, e);
    grid_.remove(wire_id(e), reg_.get<AoiCell>(e).cell);
    reg_.destroy(e);
    --population_;
}

proto::Vec3 World::clamp_to_map(uint16_t map, proto::Vec3 pos) const {
    const auto [x, z] = maps_.info(map).clamp(pos.x, pos.z);
    // ponytail: no flying NPCs; add a flag when one exists
    return {x, std::clamp(*maps_.ground(map, x, z), rules_.move.minY, rules_.move.maxY), z};
}

bool World::transfer(entt::entity e, const Destination& to) {
    const uint32_t id = wire_id(e);
    if (!maps_.owns(to.map, map::tile_of(to.pos.x, to.pos.z))) {
        // ponytail: one process owns every tile; future = save + Redirect{host,port,ticket} to the owning process.
        log::warn("transfer refused: destination tile not owned by this process",
                  {{"entity", id}, {"map", to.map}, {"x", to.pos.x}, {"z", to.pos.z}});
        return false;
    }
    const proto::Vec3 pos = clamp_to_map(to.map, to.pos); // owned, so on the map: only y changes, onto the ground
    reg_.get<MapRef>(e).map = to.map;
    reg_.get<Transform>(e) = {pos, to.heading};
    auto& cell = reg_.get<AoiCell>(e);
    grid_.remove(id, cell.cell);
    cell.cell = grid_.cell_of(to.map, pos.x, pos.z);
    grid_.insert(id, cell.cell);
    activation_move(reg_, activation_, spawner_, grid_, e);
    // Moves in flight from the old place are of the old epoch: dropped without a Correction (MapChange tells the new one).
    uint32_t epoch = 0;
    if (auto* ms = reg_.try_get<MoveState>(e)) {
        ms->budget = {0, now_};
        epoch = ++ms->epoch;
    }
    reg_.remove<Moved>(e); // a move this tick must not reach the new neighbours in an EntityMoves
    // Out of every view at once, both ways: the mover gets MapChange, then a Despawn per old neighbour; they get its
    // Despawn. The next aoi pass sends Spawns for whoever is in range now, on the new map or (a teleport within
    // the map) the same neighbours again at the new position. No Moved: no EntityMoves may show anyone where it went.
    if (const auto* link = reg_.try_get<SessionLink>(e)) {
        reg_.emplace_or_replace<Dirty>(e);
        send_(link->sessionId, proto::MapChange{to.map, pos, to.heading, epoch});
        for (uint32_t other : reg_.get<Visible>(e).ids) send_(link->sessionId, proto::Despawn{other});
        set_area(reg_, maps_, lua_.bus(), e);
    }
    leave_views(e);
    return true;
}

entt::entity World::create_npc(std::string name, std::string templateName, std::optional<uint16_t> map,
                               std::optional<proto::Vec3> pos, float heading, std::string npc,
                               const lua::Json& components) {
    const auto& info = maps_.info(map.value_or(rules_.startMap));
    const proto::Vec3 at = clamp_to_map(info.id, pos.value_or(proto::Vec3{info.spawn[0], info.spawn[1], info.spawn[2]}));
    const auto e = reg_.create();
    reg_.emplace<Npc>(e, std::move(name), std::move(templateName), std::move(npc));
    reg_.emplace<Transform>(e, at, heading);
    reg_.emplace<MapRef>(e, info.id);
    const CellKey cell = grid_.cell_of(info.id, at.x, at.z);
    reg_.emplace<AoiCell>(e, cell);
    reg_.emplace<Viewers>(e);
    reg_.emplace<ReplicatedState>(e);
    grid_.insert(wire_id(e), cell);
    activation_enter(reg_, activation_, spawner_, grid_, e);
    for (const auto& [type, value] : components.items()) lua::pool(reg_, type).emplace(e, lua::ComponentData{value});
    mark_replicated(reg_, e, defs_);
    if (components.contains(lua::kStateMachineComponent)) lua_.fsm().syncEntity(e); // as kast.add + kast.set would
    return e;
}

void World::destroy_npc(entt::entity e) {
    if (const auto* ref = reg_.try_get<SpawnRef>(e)) {
        SpawnRecord& r = spawner_.records[ref->record];
        r.alive = false;
        r.respawnAt = r.spawn->respawnSec < 0
                          ? TimePoint::max()
                          : now_ + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(r.spawn->respawnSec));
        if (r.spawn->respawnSec >= 0) spawner_.pending.push_back(ref->record);
    }
    leave_views(e);
    grid_.remove(wire_id(e), reg_.get<AoiCell>(e).cell);
    reg_.destroy(e);
}

void World::leave_views(entt::entity e) {
    const uint32_t id = wire_id(e);
    const auto unsee = [&](uint32_t viewer) { // a player that saw e
        auto& vis = reg_.get<Visible>(entt::entity{viewer});
        erase_sorted(vis.ids, id);
        erase_sorted(vis.entered, id); // gone in the tick it came into view: no component full set either
        send_(reg_.get<SessionLink>(entt::entity{viewer}).sessionId, proto::Despawn{id});
    };
    if (auto* viewers = reg_.try_get<Viewers>(e)) { // an NPC
        for (uint32_t viewer : viewers->ids) unsee(viewer);
        viewers->ids.clear();
        return;
    }
    auto& vis = reg_.get<Visible>(e); // a player: range is symmetric, so the players it sees see it
    for (uint32_t other : vis.ids) {
        if (auto* viewers = reg_.try_get<Viewers>(entt::entity{other})) erase_sorted(viewers->ids, id); // an NPC it saw
        else unsee(other);
    }
    vis.ids.clear();
    vis.entered.clear();
}

void World::spawn_due(TimePoint now) {
    spawner_system(reg_, spawner_, rules_.maxNpcs, now, [this](const SpawnRecord& r) {
        const auto& s = *r.spawn;
        const lua::NpcDef* def = lua_.npc(s.npc); // load_scripts refused the start without one
        if (!def) throw std::logic_error("spawn \"" + s.id + "\": npc \"" + s.npc + "\" not loaded");
        return create_npc(def->name, def->templateName, r.map, proto::Vec3{s.pos[0], s.pos[1], s.pos[2]}, s.heading,
                          def->name, def->components); // fresh definition data on every respawn
    });
}

void World::remove_npcs() {
    const auto view = reg_.view<Npc>();
    for (const std::vector<entt::entity> npcs(view.begin(), view.end()); entt::entity e : npcs) destroy_npc(e);
}

void World::push_move(entt::entity e, const proto::Move& m) {
    if (reg_.valid(e)) reg_.get<PendingMoves>(e).moves.push_back(m);
}

uint16_t World::map_of(entt::entity e) const { return reg_.get<MapRef>(e).map; }

void World::push_say(entt::entity e, proto::Say say) {
    if (reg_.valid(e)) reg_.get<PendingSay>(e).says.push_back(std::move(say));
}

void World::push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId) {
    if (reg_.valid(e)) reg_.get<PendingClientEvents>(e).events.push_back({std::move(name), std::move(data), requestId});
}

void World::push_interact(entt::entity e, uint32_t objectId) {
    if (reg_.valid(e)) reg_.get<PendingInteracts>(e).objectIds.push_back(objectId);
}

// area runs right after movement: a player who walked into a portal is transferred before aoi, so this tick's view
// diff already sends the Spawns on the new map (MapChange and the old Despawns went out in transfer).
// The spawner runs right before Lua, so a system's update already sees (and can move) the NPCs due this tick; like the
// ones Lua creates, players get their Spawn in the next tick's aoi diff.
// Activation runs once, after Lua's NPC moves are re-bucketed: every player and NPC that changed tile this tick (a
// tile loading now has its NPCs spawned by the next tick's spawner). Transfers and logins activate at once.
// Lua runs after movement and chat (it sees this tick's positions) and before replication and save (its changes go
// out and get saved this tick). aoi stays before both replications, so a Spawn always precedes component data; the
// NPCs Lua creates or moves out of a view show up in the next tick's aoi diff. The events Lua published are delivered
// right after update; client events go out after component replication, so a client already has the Spawn and the
// latest component values they may refer to.
void World::tick(TimePoint now) {
    now_ = now;
    movement_system(reg_, rules_.move, maps_, send_, now);
    for (const auto& [e, portal] : area_system(reg_, maps_, lua_.bus()))
        transfer(e, {portal.map, {portal.pos[0], portal.pos[1], portal.pos[2]}, portal.heading});
    aoi_system(reg_, grid_, send_);
    chat_system(reg_, rules_.chat, mutes_, send_, sendMany_, now, std::chrono::system_clock::now());
    client_event_in_system(reg_, lua_.bus(), now); // delivered by the drain after this tick's Lua update
    interact_system(reg_, maps_, lua_.bus());      // likewise
    spawn_due(now);
    lua_.update(rules_.tickSeconds);
    lua_.drain_events();
    aoi_rebucket(reg_, grid_); // NPCs Lua moved: in their new cell for the next tick's view diff
    activation_system(reg_, activation_, spawner_, grid_);
    replication_system(reg_, send_);
    component_replication_system(reg_, defs_, send_, sendMany_);
    client_event_system(reg_, lua_.take_client_events(), send_, sendMany_);
    save_system(reg_, defs_, save_, rules_.saveInterval, now);
    end_of_tick_system(reg_);
}

} // namespace kast::world
