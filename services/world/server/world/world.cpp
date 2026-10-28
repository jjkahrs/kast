#include "world.h"
#include "components.h"
#include "../core/log.h"

#include <algorithm>
#include <format>
#include <set>
#include <stdexcept>

namespace kast::world {

World::World(WorldRules rules, const Maps& maps, Send send, SendMany sendMany, Save save, lua::Defs defs)
    : rules_(rules), maps_(maps), send_(std::move(send)), sendMany_(std::move(sendMany)), save_(std::move(save)),
      defs_(std::move(defs)),
      realm_(realmReg_, {},
             {.create = {}, // the realm has no entities: nothing to create, destroy, transfer or spawn
              .destroy = {},
              .clamp = [this](uint16_t map, proto::Vec3 pos) { return clamp_to_map(map, pos); },
              .map = [this](uint16_t id) { return maps_.by_id(id); },
              .ground = [this](uint16_t map, float x, float z) { return maps_.ground(map, x, z); },
              .path = [this](uint16_t map, proto::Vec3 from, proto::Vec3 to) { return maps_.nav().path(map, from, to); },
              .transfer = {},
              .spawn = {},
              .count = [this] { return shared_.npcs; },
              .post = [this](lua::Post post) { realmPosts_.push_back(std::move(post)); },
              .max = rules.maxNpcs}) {
    // Delivered on each map's bus at its next drain, as when every Lua system shared the game machines' bus.
    realmChanged_ = realm_.bus().subscribe(lua::kStateChangedEvent, [this](const nlohmann::json& p) {
        for (auto& [id, ctx] : contexts_) ctx->bus().publish(lua::kStateChangedEvent, p, "realm");
    });
    log::debug("realm context started");
}

World::~World() = default;

MapContext& World::context(uint16_t map) {
    if (auto it = contexts_.find(map); it != contexts_.end()) return *it->second;
    MapContext& ctx = *contexts_
                           .emplace(map, std::make_unique<MapContext>(map, rules_, maps_, shared_, send_, sendMany_,
                                                                      save_, defs_, now_))
                           .first->second;
    if (luaRoot_) // the same files every context loaded: any error was a startup error already
        for (const std::string& e : ctx.load_scripts(*luaRoot_, gameMachines_, realm_.fsm().game_states()))
            log::error("Lua error", {{"map", map}, {"error", e}});
    return ctx;
}

MapContext* World::find(entt::entity e) const {
    for (const auto& [id, ctx] : contexts_)
        if (ctx->registry().valid(e)) return ctx.get();
    return nullptr;
}

std::vector<std::string> World::load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines) {
    context(rules_.startMap); // created before luaRoot_ is set: loaded below with the others
    luaRoot_ = luaRoot;
    gameMachines_ = std::move(gameMachines);
    std::vector<std::string> errors = realm_.load_game_machines(luaRoot, gameMachines_);
    for (auto& [id, ctx] : contexts_)
        for (std::string& e : ctx->load_scripts(luaRoot, gameMachines_, realm_.fsm().game_states()))
            errors.push_back(std::move(e));
    std::vector<std::string> unique; // every context reads the same files, and the realm the same fsms/
    for (std::string& e : errors)
        if (std::ranges::find(unique, e) == unique.end()) unique.push_back(std::move(e));
    return unique;
}

void World::shutdown_scripts() {
    for (auto& [id, ctx] : contexts_) ctx->shutdown_scripts();
    realm_.shutdown();
    serial(false); // the broadcasts reach the other maps' players before the despawns
}

std::vector<lua::SystemStatus> World::script_status() const {
    std::vector<lua::SystemStatus> merged = context(rules_.startMap).script_status();
    for (const auto& [id, ctx] : contexts_)
        for (const lua::SystemStatus& s : ctx->script_status())
            for (lua::SystemStatus& m : merged)
                if (m.path == s.path && m.ok && !s.ok) m = s;
    return merged;
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
    const entt::entity e = context(map).spawn(sessionId, row, pos, heading, now);
    serial(false);
    return e;
}

void World::despawn(entt::entity e) {
    if (MapContext* ctx = find(e)) {
        ctx->despawn(e);
        serial(false);
    }
}

entt::entity World::create_npc(std::string name, std::string templateName, std::optional<uint16_t> map,
                               std::optional<proto::Vec3> pos, float heading, std::string npc,
                               const lua::Json& components) {
    const entt::entity e = context(map.value_or(rules_.startMap))
                               .create_npc(std::move(name), std::move(templateName), pos, heading, std::move(npc),
                                           components);
    serial(false);
    return e;
}

void World::destroy_npc(entt::entity e) {
    if (MapContext* ctx = find(e)) {
        ctx->destroy_npc(e);
        serial(false);
    }
}

void World::remove_npcs() {
    for (auto& [id, ctx] : contexts_) ctx->remove_npcs();
}

bool World::transfer(entt::entity e, const Destination& to) {
    const bool moved = move_to(e, to);
    serial(false);
    return moved;
}

bool World::move_to(entt::entity e, const Destination& to) {
    MapContext* from = find(e);
    if (!from) return false;
    if (!maps_.owns(to.map, map::tile_of(to.pos.x, to.pos.z))) {
        // ponytail: one process owns every tile; future = save + Redirect{host,port,ticket} to the owning process.
        log::warn("transfer refused: destination tile not owned by this process",
                  {{"entity", wire_id(e)}, {"map", to.map}, {"x", to.pos.x}, {"z", to.pos.z}});
        return false;
    }
    if (to.map == from->map()) {
        from->transfer(e, to);
        return true;
    }
    MapContext& dest = context(to.map);
    const lua::Carried carried = from->leave_map(e);
    if (!from->registry().valid(e)) return false; // onMapLeave destroyed it
    const Destination at{to.map, clamp_to_map(to.map, to.pos), to.heading}; // owned, so on the map: y onto the ground
    from->depart(e, at);
    move_entity(from->registry(), dest.registry(), e, defs_);
    dest.arrive(e, at);
    dest.enter_map(e, carried);
    return true;
}

void World::serial(bool endOfTick) {
    std::vector<std::pair<uint16_t, MapContext::Outbox>> boxes; // taken first: what this work queues waits for the next
    for (auto& [id, ctx] : contexts_)
        if (MapContext::Outbox box = ctx->take_outbox(); !box.empty()) boxes.emplace_back(id, std::move(box));
    // Every context reads the same mirror, so the same change from several of them is one intent: applied once.
    std::set<std::pair<std::string, std::string>> gameChanges;
    for (auto& [from, box] : boxes) {
        for (const auto& [e, to] : box.transfers) move_to(e, to); // e may be gone since (logged out, destroyed)
        for (MapContext::Create& c : box.creates)
            context(c.map).create_npc(std::move(c.name), std::move(c.templateName), c.pos, c.heading, std::move(c.npc),
                                      c.components, c.id);
        for (const auto& [map, record] : box.respawns) context(map).spawn_destroyed(record);
        for (const auto& [machine, to] : box.gameChanges) {
            if (!gameChanges.emplace(machine, to).second) continue;
            try {
                realm_.fsm().change(machine, to); // checked against the mirror; the realm warns if no longer allowed
            } catch (const std::exception& ex) {
                log::warn(std::format("Game machine change to \"{}\" skipped: {}", to, ex.what()));
            }
        }
        for (const auto& [id, ctx] : contexts_)
            if (id != from && !box.broadcasts.empty()) ctx->send_client_events(box.broadcasts);
        for (lua::Post& post : box.posts) posts_.emplace_back(from, std::move(post));
    }
    if (std::vector<lua::ClientEvent> events = realm_.take_client_events(); !events.empty())
        for (auto& [id, ctx] : contexts_) ctx->send_client_events(events); // each context sends to its own players
    for (lua::Post& post : std::exchange(realmPosts_, {})) posts_.emplace_back("realm", std::move(post));
    if (endOfTick)
        for (const auto& [from, post] : std::exchange(posts_, {})) deliver(from, post);
    for (auto& [id, ctx] : contexts_) ctx->mirror_game_machines(realm_.fsm().game_states());
}

void World::deliver(const nlohmann::json& from, const lua::Post& post) {
    const nlohmann::json payload{{"from", from}, {"data", post.data}};
    const std::string publisher = from.is_string() ? "realm" : "map " + from.dump();
    switch (post.to) {
    case lua::Post::To::map: context(post.map).bus().publish(post.event, payload, publisher); break;
    case lua::Post::To::realm: realm_.bus().publish(post.event, payload, publisher); break;
    case lua::Post::To::all:
        for (auto& [id, ctx] : contexts_) ctx->bus().publish(post.event, payload, publisher);
        realm_.bus().publish(post.event, payload, publisher);
        break;
    }
}

void World::push_move(entt::entity e, const proto::Move& m) {
    if (MapContext* ctx = find(e)) ctx->push_move(e, m);
}

void World::push_say(entt::entity e, proto::Say say) {
    if (MapContext* ctx = find(e)) ctx->push_say(e, std::move(say));
}

void World::push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId) {
    if (MapContext* ctx = find(e)) ctx->push_client_event(e, std::move(name), std::move(data), requestId);
}

void World::push_interact(entt::entity e, uint32_t objectId) {
    if (MapContext* ctx = find(e)) ctx->push_interact(e, objectId);
}

uint16_t World::map_of(entt::entity e) const { return find(e)->map(); }

size_t World::population() const {
    size_t n = 0;
    for (const auto& [id, ctx] : contexts_) n += ctx->population();
    return n;
}

// The contexts run one after another, each step for every context before the next step, with a serial point between
// (the parallel map update, phase 3, runs each step's contexts on worker threads instead):
// - the realm's bus first: C++ subscribers, and the game machines' fsm.changed for every map's drain this tick;
// - movement and areas: a portal onto another map is queued, and moved at the serial point right after, so the
//   destination's aoi pass sends its Spawns this tick (and its area events reach both buses this tick);
// - the rest of the tick (MapContext::tick_rest), in which Lua's kast.transfer, kast.create and game machine changes
//   reaching past its map are queued for the serial point at the end of the tick, and kast.world.post for the next.
void World::tick(TimePoint now) {
    now_ = now;
    realm_.drain_events();
    serial(false); // work C++ calls queued since the last tick
    for (auto& [id, ctx] : contexts_) ctx->tick_move(now);
    serial(false);
    for (auto& [id, ctx] : contexts_) ctx->tick_rest(now, mutes_);
    serial(true);
}

} // namespace kast::world
