#include "zone_edit.h"

#include "terrain_edit.h" // ground_y

#include <scene_io.h> // jsonNumber

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <utility>

namespace zone {
namespace {

double num(float v) { return kast::jsonNumber(v); }

float as_float(const Json& v) { return v.is_number() ? v.get<float>() : 0.0f; }

// name if free in obj, else name_2, name_3...
std::string unique_key(const Json& obj, const std::string& name) {
    std::string candidate = name;
    for (int n = 2; obj.is_object() && obj.contains(candidate); ++n) candidate = name + "_" + std::to_string(n);
    return candidate;
}

bool uses_path(const Json& file, const std::string& name) {
    const Json& spawns = file.value("spawns", Json::array());
    return std::any_of(spawns.begin(), spawns.end(), [&](const Json& s) { return s.value("path", Json()) == name; });
}

} // namespace

Json point_json(Point p) { return Json::array({num(p[0]), num(p[1])}); }
Json vec3_json(const Vec3& v) { return Json::array({num(v[0]), num(v[1]), num(v[2])}); }

void set_instance(ZoneState& state, bool instance, const kast::map::Portal& exit) {
    Json& m = state.manifest;
    if (!instance) {
        for (const char* key : {"type", "maxPlayers", "emptyTimeoutSec", "exit"}) m.erase(key);
        return;
    }
    m["type"] = "instance";
    if (!m.contains("exit")) set_exit(state, exit);
}

void set_max_players(ZoneState& state, std::optional<uint32_t> players) {
    if (players) state.manifest["maxPlayers"] = *players;
    else state.manifest.erase("maxPlayers");
}

void set_empty_timeout(ZoneState& state, uint32_t seconds) { state.manifest["emptyTimeoutSec"] = seconds; }

void set_exit(ZoneState& state, const kast::map::Portal& exit) {
    state.manifest["exit"] = {{"map", unsigned(exit.map)}, {"pos", vec3_json(exit.pos)}, {"heading", num(exit.heading)}};
}

Json& areas_list(ZoneState& state) {
    if (!state.areas.is_object()) state.areas = {{"version", 1}, {"areas", Json::array()}};
    if (!state.areas.contains("areas") || !state.areas["areas"].is_array()) state.areas["areas"] = Json::array();
    return state.areas["areas"];
}

std::optional<size_t> find_area(const ZoneState& state, const std::string& id) {
    if (!state.areas.is_object() || !state.areas.contains("areas")) return std::nullopt;
    const Json& list = state.areas["areas"];
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].value("id", Json()) == id) return i;
    return std::nullopt;
}

std::string unique_area_id(const ZoneState& state, const std::string& base) {
    std::string id = base;
    for (int n = 2; find_area(state, id); ++n) id = base + "_" + std::to_string(n);
    return id;
}

size_t add_rect_area(ZoneState& state, const std::string& id, Point a, Point b, int priority) {
    Json& list = areas_list(state);
    list.push_back({{"id", id},
                    {"name", id},
                    {"priority", priority},
                    {"rect", Json::array({num(std::min(a[0], b[0])), num(std::min(a[1], b[1])), num(std::max(a[0], b[0])),
                                          num(std::max(a[1], b[1]))})}});
    return list.size() - 1;
}

size_t add_polygon_area(ZoneState& state, const std::string& id, const std::vector<Point>& points, int priority) {
    Json polygon = Json::array();
    for (const Point& p : points) polygon.push_back(point_json(p));
    Json& list = areas_list(state);
    list.push_back({{"id", id}, {"name", id}, {"priority", priority}, {"polygon", std::move(polygon)}});
    return list.size() - 1;
}

std::vector<Point> area_points(const Json& area) {
    if (const Json& r = area.value("rect", Json()); r.is_array() && r.size() == 4) {
        const float x0 = as_float(r[0]), z0 = as_float(r[1]), x1 = as_float(r[2]), z1 = as_float(r[3]);
        return {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
    }
    std::vector<Point> points;
    for (const Json& p : area.value("polygon", Json::array()))
        if (p.is_array() && p.size() == 2) points.push_back({as_float(p[0]), as_float(p[1])});
    return points;
}

void move_area_vertex(Json& area, size_t vertex, Point to) {
    if (area.contains("rect")) {
        std::vector<Point> c = area_points(area);
        if (vertex >= c.size()) return;
        // Corners x0z0, x1z0, x1z1, x0z1: which of x0/x1 and z0/z1 this one owns.
        float x[2] = {c[0][0], c[2][0]}, z[2] = {c[0][1], c[2][1]};
        x[vertex == 1 || vertex == 2 ? 1 : 0] = to[0];
        z[vertex >= 2 ? 1 : 0] = to[1];
        area["rect"] = Json::array({num(std::min(x[0], x[1])), num(std::min(z[0], z[1])), num(std::max(x[0], x[1])),
                                    num(std::max(z[0], z[1]))});
    } else if (area.contains("polygon") && vertex < area["polygon"].size()) {
        area["polygon"][vertex] = point_json(to);
    }
}

void move_area(Json& area, float dx, float dz) {
    if (area.contains("rect")) {
        const auto c = area_points(area);
        area["rect"] = Json::array({num(c[0][0] + dx), num(c[0][1] + dz), num(c[2][0] + dx), num(c[2][1] + dz)});
    } else {
        auto points = area_points(area);
        Json polygon = Json::array();
        for (const Point& p : points) polygon.push_back(point_json({p[0] + dx, p[1] + dz}));
        area["polygon"] = std::move(polygon);
    }
}

void rename_area(ZoneState& state, size_t index, const std::string& id) {
    Json& list = areas_list(state);
    const Json old = list[index].value("id", Json());
    for (Json& a : list)
        if (a.value("parent", Json()) == old) a["parent"] = id;
    list[index]["id"] = id;
}

void delete_area(ZoneState& state, size_t index) {
    Json& list = areas_list(state);
    const Json old = list[index].value("id", Json());
    list.erase(index);
    for (Json& a : list)
        if (a.value("parent", Json()) == old) a.erase("parent");
}

std::vector<SpawnRef> all_spawns(const ZoneState& state) {
    std::vector<SpawnRef> refs;
    for (const auto& [tile, file] : state.spawns) {
        const Json& spawns = file.value("spawns", Json::array());
        for (size_t i = 0; i < spawns.size(); ++i) refs.push_back({tile, i});
    }
    return refs;
}

const Json& spawn_json(const ZoneState& state, SpawnRef ref) { return state.spawns.at(ref.tile)["spawns"][ref.index]; }
Json& spawn_json(ZoneState& state, SpawnRef ref) { return state.spawns.at(ref.tile)["spawns"][ref.index]; }

std::optional<SpawnRef> find_spawn(const ZoneState& state, const std::string& id) {
    for (const SpawnRef& ref : all_spawns(state))
        if (spawn_json(state, ref).value("id", Json()) == id) return ref;
    return std::nullopt;
}

std::string unique_spawn_id(const ZoneState& state, const std::string& base) {
    std::string id = base;
    for (int n = 2; find_spawn(state, id); ++n) id = base + "_" + std::to_string(n);
    return id;
}

Json& spawn_file(ZoneState& state, TileKey tile) {
    Json& file = state.spawns[tile];
    if (!file.is_object()) file = {{"version", 2}, {"paths", Json::object()}, {"spawns", Json::array()}};
    if (!file.contains("spawns")) file["spawns"] = Json::array();
    return file;
}

SpawnRef add_spawn(ZoneState& state, const std::string& id, const std::string& npc, const Vec3& pos, float heading,
                   float respawnSec) {
    const TileKey tile = kast::map::tile_of(pos[0], pos[2]);
    Json& spawns = spawn_file(state, tile)["spawns"];
    spawns.push_back({{"id", id}, {"npc", npc}, {"pos", vec3_json(pos)}, {"heading", num(heading)}, {"respawnSec", num(respawnSec)}});
    return {tile, spawns.size() - 1};
}

SpawnRef move_spawn(ZoneState& state, SpawnRef ref, const Vec3& pos) {
    const TileKey tile = kast::map::tile_of(pos[0], pos[2]);
    if (tile == ref.tile) {
        spawn_json(state, ref)["pos"] = vec3_json(pos);
        return ref;
    }
    Json& from = state.spawns.at(ref.tile);
    Json spawn = from["spawns"][ref.index];
    from["spawns"].erase(ref.index);
    spawn["pos"] = vec3_json(pos);
    Json& to = spawn_file(state, tile);
    if (const Json path = spawn.value("path", Json()); path.is_string() && from.contains("paths") && from["paths"].contains(path)) {
        const std::string name = path.get<std::string>();
        const Json route = from["paths"][name];
        if (!to.contains("paths")) to["paths"] = Json::object();
        std::string target = name;
        if (to["paths"].contains(name) && to["paths"][name] != route) target = unique_key(to["paths"], name);
        to["paths"][target] = route;
        spawn["path"] = target;
        if (!uses_path(from, name)) from["paths"].erase(name); // moved with its last user
    }
    to["spawns"].push_back(std::move(spawn));
    return {tile, to["spawns"].size() - 1};
}

void delete_spawn(ZoneState& state, SpawnRef ref) { state.spawns.at(ref.tile)["spawns"].erase(ref.index); }

std::string add_path(ZoneState& state, SpawnRef ref, const std::string& name, const std::vector<Vec3>& points,
                     float wait, bool loop) {
    Json& file = spawn_file(state, ref.tile);
    if (!file.contains("paths") || !file["paths"].is_object()) file["paths"] = Json::object();
    const std::string key = unique_key(file["paths"], name);
    Json route = Json::array();
    for (const Vec3& p : points) route.push_back(vec3_json(p));
    file["paths"][key] = {{"loop", loop}, {"wait", num(wait)}, {"points", std::move(route)}};
    spawn_json(state, ref)["path"] = key;
    return key;
}

void delete_path(ZoneState& state, TileKey tile, const std::string& name) {
    Json& file = spawn_file(state, tile);
    if (file.contains("paths")) file["paths"].erase(name);
    for (Json& s : file["spawns"])
        if (s.value("path", Json()) == name) s.erase("path");
}

Json* spawn_path(ZoneState& state, SpawnRef ref) {
    Json& file = state.spawns.at(ref.tile);
    const Json name = spawn_json(state, ref).value("path", Json());
    if (!name.is_string() || !file.contains("paths") || !file["paths"].contains(name)) return nullptr;
    return &file["paths"][name.get<std::string>()];
}

void add_path_point(ZoneState& state, SpawnRef ref, const Vec3& point) {
    if (Json* route = spawn_path(state, ref)) {
        (*route)["points"].push_back(vec3_json(point));
        return;
    }
    const Json& s = spawn_json(state, ref);
    const Json& pos = s.value("pos", Json::array({0, 0, 0}));
    const Vec3 start{as_float(pos[0]), as_float(pos[1]), as_float(pos[2])};
    add_path(state, ref, s.value("id", std::string("spawn")) + "_path", {start, point}, 0, true);
}

namespace {

namespace km = kast::map;

Json& objects_list(ZoneState& state, TileKey tile) {
    Json& file = state.objects[tile];
    if (!file.is_object()) file = {{"version", 1}, {"objects", Json::array()}};
    if (!file.contains("objects") || !file["objects"].is_array()) file["objects"] = Json::array();
    return file["objects"];
}

Json& interactables_list(ZoneState& state, TileKey tile) {
    Json& file = state.interactables[tile];
    if (!file.is_object()) file = {{"version", 1}, {"interactables", Json::array()}};
    if (!file.contains("interactables") || !file["interactables"].is_array()) file["interactables"] = Json::array();
    return file["interactables"];
}

// Drops tile's files whose lists are empty (save then deletes them).
void prune(ZoneState& state, TileKey tile) {
    if (const auto it = state.objects.find(tile); it != state.objects.end() && it->second.value("objects", Json::array()).empty())
        state.objects.erase(it);
    if (const auto it = state.interactables.find(tile);
        it != state.interactables.end() && it->second.value("interactables", Json::array()).empty())
        state.interactables.erase(it);
}

uint32_t id_of(const Json& j) { return j.value("id", Json()).is_number_unsigned() ? j["id"].get<uint32_t>() : 0; }

// Removes interactable id's server entry from tile's file; returns it (null if none).
Json take_interactable(ZoneState& state, TileKey tile, uint32_t id) {
    const auto it = state.interactables.find(tile);
    if (it == state.interactables.end()) return Json();
    Json& list = it->second["interactables"];
    for (size_t i = 0; i < list.size(); ++i)
        if (id_of(list[i]) == id) {
            Json entry = std::move(list[i]);
            list.erase(i);
            return entry;
        }
    return Json();
}

} // namespace

std::vector<ObjectRef> all_objects(const ZoneState& state) {
    std::vector<ObjectRef> refs;
    for (const auto& [tile, file] : state.objects) {
        const Json& list = file.value("objects", Json::array());
        for (size_t i = 0; i < list.size(); ++i) refs.push_back({tile, i});
    }
    return refs;
}

const Json& object_json(const ZoneState& state, ObjectRef ref) { return state.objects.at(ref.tile)["objects"][ref.index]; }
Json& object_json(ZoneState& state, ObjectRef ref) { return state.objects.at(ref.tile)["objects"][ref.index]; }

km::ObjectType object_type(const Json& object) {
    const Json& type = object.value("type", Json());
    return type == "obstacle" ? km::ObjectType::Obstacle : type == "interactable" ? km::ObjectType::Interactable : km::ObjectType::Visual;
}

uint32_t next_interactable_id(const ZoneState& state) {
    uint32_t top = 0;
    for (const auto& [tile, file] : state.objects)
        for (const Json& o : file.value("objects", Json::array())) top = std::max(top, id_of(o));
    for (const auto& [tile, file] : state.interactables)
        for (const Json& e : file.value("interactables", Json::array())) top = std::max(top, id_of(e));
    return top + 1;
}

Json* interactable_json(ZoneState& state, TileKey tile, uint32_t id) {
    return const_cast<Json*>(interactable_json(std::as_const(state), tile, id));
}

const Json* interactable_json(const ZoneState& state, TileKey tile, uint32_t id) {
    const auto it = state.interactables.find(tile);
    if (it == state.interactables.end() || !it->second.contains("interactables")) return nullptr;
    for (const Json& e : it->second["interactables"])
        if (id_of(e) == id) return &e;
    return nullptr;
}

Json default_shape() { return {{"box", {{"halfExtents", Json::array({0.5, 0.5, 0.5})}}}}; }

ObjectRef add_object(ZoneState& state, km::ObjectType type, const std::string& prefab, const Vec3& pos, float yaw, float scale) {
    const TileKey tile = km::tile_of(pos[0], pos[2]);
    Json& list = objects_list(state, tile);
    list.push_back({{"type", "visual"}, {"prefab", prefab}, {"pos", vec3_json(pos)}, {"yaw", num(yaw)}, {"scale", num(scale)}});
    const ObjectRef ref{tile, list.size() - 1};
    set_object_type(state, ref, type);
    return ref;
}

ObjectRef move_object(ZoneState& state, ObjectRef ref, const Vec3& pos) {
    const TileKey tile = km::tile_of(pos[0], pos[2]);
    if (tile == ref.tile) {
        object_json(state, ref)["pos"] = vec3_json(pos);
        if (object_type(object_json(state, ref)) == km::ObjectType::Interactable) sync_interactable(state, ref);
        return ref;
    }
    Json object = std::move(object_json(state, ref));
    state.objects.at(ref.tile)["objects"].erase(ref.index);
    object["pos"] = vec3_json(pos);
    const bool interactable = object_type(object) == km::ObjectType::Interactable;
    Json entry = interactable ? take_interactable(state, ref.tile, id_of(object)) : Json();
    prune(state, ref.tile);
    Json& list = objects_list(state, tile);
    list.push_back(std::move(object));
    const ObjectRef moved{tile, list.size() - 1};
    if (interactable) {
        if (!entry.is_null()) interactables_list(state, tile).push_back(std::move(entry));
        sync_interactable(state, moved);
    }
    return moved;
}

void delete_object(ZoneState& state, ObjectRef ref) {
    const Json object = object_json(state, ref);
    state.objects.at(ref.tile)["objects"].erase(ref.index);
    if (object_type(object) == km::ObjectType::Interactable) take_interactable(state, ref.tile, id_of(object));
    prune(state, ref.tile);
}

void set_object_type(ZoneState& state, ObjectRef ref, km::ObjectType type) {
    Json& o = object_json(state, ref);
    const km::ObjectType was = object_type(o);
    o["type"] = km::object_type_name(type);
    if (type != km::ObjectType::Visual && !o.contains("shape")) o["shape"] = default_shape();
    if (was == km::ObjectType::Interactable && type != km::ObjectType::Interactable) {
        take_interactable(state, ref.tile, id_of(o));
        o.erase("id");
        o.erase("name");
        prune(state, ref.tile);
    } else if (type == km::ObjectType::Interactable && was != km::ObjectType::Interactable) {
        o["id"] = next_interactable_id(state);
        sync_interactable(state, ref);
    }
}

void sync_interactable(ZoneState& state, ObjectRef ref) {
    const Json& o = object_json(state, ref);
    const uint32_t id = id_of(o);
    Json* entry = interactable_json(state, ref.tile, id);
    if (!entry) {
        const std::string stem = std::filesystem::path(o.value("prefab", std::string("object"))).stem().string();
        Json& list = interactables_list(state, ref.tile);
        list.push_back({{"id", id}, {"range", 3}, {"event", stem + ".activated"}});
        entry = &list.back();
    }
    for (const char* key : {"pos", "yaw", "shape", "name"}) {
        if (o.contains(key)) (*entry)[key] = o[key];
        else entry->erase(key);
    }
}

void set_interactable_id(ZoneState& state, ObjectRef ref, uint32_t id) {
    Json& o = object_json(state, ref);
    if (Json* entry = interactable_json(state, ref.tile, id_of(o))) (*entry)["id"] = id;
    o["id"] = id;
    sync_interactable(state, ref);
}

float ScatterRng::next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return float(state_ >> 8) * (1.0f / 16777216.0f);
}

size_t scatter(const km::MapInfo& info, ZoneState& state, const Scatter& brush, ScatterRng& rng, float x, float z) {
    if (brush.prefabs.empty()) return 0;
    size_t placed = 0;
    for (int i = 0; i < brush.count; ++i) {
        // Every draw happens whether or not the point lands, so a dab's sequence never depends on the map.
        const float r = brush.radius * std::sqrt(rng.next()), a = 2 * std::numbers::pi_v<float> * rng.next();
        const std::string& prefab = brush.prefabs[std::min(size_t(rng.next() * float(brush.prefabs.size())), brush.prefabs.size() - 1)];
        const float turn = rng.next();
        const float yaw = brush.randomYaw ? 2 * std::numbers::pi_v<float> * turn : 0.0f;
        const float scale = brush.scaleMin + (brush.scaleMax - brush.scaleMin) * rng.next();
        const float px = x + r * std::cos(a), pz = z + r * std::sin(a);
        const std::optional<float> y = ground_y(info, state, px, pz);
        if (!y) continue;
        add_object(state, km::ObjectType::Visual, prefab, {px, *y, pz}, yaw, std::max(scale, 0.01f));
        ++placed;
    }
    return placed;
}

size_t erase_visuals(ZoneState& state, float x, float z, float radius) {
    size_t erased = 0;
    for (auto it = state.objects.begin(); it != state.objects.end();) {
        Json& list = it->second["objects"];
        for (size_t i = list.size(); i-- > 0;) {
            const Json& pos = list[i].value("pos", Json());
            if (object_type(list[i]) != km::ObjectType::Visual || !pos.is_array() || pos.size() != 3) continue;
            if (std::hypot(as_float(pos[0]) - x, as_float(pos[2]) - z) > radius) continue;
            list.erase(i);
            ++erased;
        }
        it = list.empty() ? state.objects.erase(it) : std::next(it);
    }
    return erased;
}

} // namespace zone
