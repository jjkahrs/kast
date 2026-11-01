#include "zone_edit.h"

#include "map_io.h" // kMaxMapTiles

#include <algorithm>

namespace mapedit {
namespace {

namespace km = kast::map;

// The zones' JSON list, or null without one.
Json* zones_json(MapState& state) {
    return state.zones.is_object() && state.zones.contains("zones") && state.zones["zones"].is_array()
               ? &state.zones["zones"]
               : nullptr;
}

TileKey offset(TileKey t, TileKey d) { return {t.x + d.x, t.z + d.z}; }

} // namespace

km::Zones map_zones(const km::MapInfo& info, const MapState& state, const std::string& folder,
                    std::vector<std::string>* problems) {
    if (state.zones.is_null()) return km::implicit_zones(info, folder);
    std::string err;
    std::optional<km::Zones> zones = km::read_zones(state.zones, err);
    if (!zones) {
        if (problems) problems->push_back(err);
        return {};
    }
    const std::vector<std::string> found = km::validate_zones(*zones, info);
    if (problems) problems->insert(problems->end(), found.begin(), found.end());
    return std::move(*zones);
}

Json& zones_list(const km::MapInfo& info, MapState& state, const std::string& folder) {
    if (state.zones.is_null()) {
        Json zone = {{"id", folder}, {"name", info.name.empty() ? folder : info.name}, {"tiles", state.manifest["tiles"]}};
        state.zones = {{"version", 1}, {"zones", Json::array({std::move(zone)})}};
    }
    if (!state.zones.is_object()) state.zones = {{"version", 1}};
    if (!zones_json(state)) state.zones["zones"] = Json::array();
    return state.zones["zones"];
}

std::optional<size_t> find_zone(const MapState& state, const std::string& id) {
    if (!state.zones.is_object() || !state.zones.contains("zones") || !state.zones["zones"].is_array()) return std::nullopt;
    const Json& list = state.zones["zones"];
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].is_object() && list[i].value("id", Json()) == id) return i;
    return std::nullopt;
}

std::string unique_zone_id(const MapState& state, const std::string& base) {
    std::string id = base;
    for (int n = 2; find_zone(state, id); ++n) id = base + "_" + std::to_string(n);
    return id;
}

TileSet zone_tiles(const Json& zone) {
    TileSet tiles;
    const Json rects = zone.is_object() ? zone.value("tiles", Json()) : Json();
    for (const Json& r : rects.is_array() ? rects : Json::array()) {
        if (!r.is_array() || r.size() != 4 ||
            !std::all_of(r.begin(), r.end(), [](const Json& v) { return v.is_number_integer(); }))
            continue;
        const int64_t x0 = r[0], z0 = r[1], x1 = r[2], z1 = r[3];
        if (x0 > x1 || z0 > z1 || (x1 - x0 + 1) * (z1 - z0 + 1) > int64_t(kMaxMapTiles) * kMaxMapTiles) continue;
        for (int64_t z = z0; z <= z1; ++z)
            for (int64_t x = x0; x <= x1; ++x) tiles.insert({int32_t(x), int32_t(z)});
    }
    return tiles;
}

size_t add_zone(const km::MapInfo& info, MapState& state, const std::string& folder, const std::string& id,
                const std::string& name) {
    Json& list = zones_list(info, state, folder);
    Json zone = {{"id", id}};
    if (!name.empty()) zone["name"] = name;
    zone["tiles"] = Json::array();
    list.push_back(std::move(zone));
    return list.size() - 1;
}

size_t paint_zone(const km::MapInfo& info, MapState& state, size_t zone, const TileSet& tiles) {
    Json* list = zones_json(state);
    if (!list || zone >= list->size()) return 0;
    std::vector<TileSet> sets;
    for (const Json& z : *list) sets.push_back(zone_tiles(z));
    std::vector<bool> changed(sets.size());
    size_t moved = 0;
    for (const TileKey t : tiles) {
        if (!info.has_tile(t) || sets[zone].contains(t)) continue;
        for (size_t i = 0; i < sets.size(); ++i)
            if (sets[i].erase(t)) changed[i] = true;
        sets[zone].insert(t);
        changed[zone] = true;
        ++moved;
    }
    for (size_t i = 0; i < sets.size(); ++i)
        if (changed[i]) (*list)[i]["tiles"] = tile_rects(sets[i]);
    return moved;
}

void rename_zone(MapState& state, size_t zone, const std::string& id) {
    Json* list = zones_json(state);
    if (!list || zone >= list->size()) return;
    const std::string old = (*list)[zone].value("id", std::string());
    const std::string onDisk = zone_scene_id(state, old);
    state.zoneRenames.erase(old);
    if (onDisk != id) state.zoneRenames[id] = onDisk;
    (*list)[zone]["id"] = id;
}

std::optional<std::string> delete_zone(MapState& state, size_t zone) {
    Json* list = zones_json(state);
    if (!list || zone >= list->size()) return std::nullopt;
    const TileSet own = zone_tiles((*list)[zone]);
    std::vector<TileSet> sets;
    for (const Json& z : *list) sets.push_back(zone_tiles(z));
    std::optional<size_t> to;
    for (const TileKey t : own) {
        for (const TileKey d : kZoneNeighbours)
            for (size_t i = 0; i < sets.size() && !to; ++i)
                if (i != zone && sets[i].contains(offset(t, d))) to = i;
        if (to) break;
    }
    std::optional<std::string> id;
    if (to) {
        sets[*to].insert(own.begin(), own.end());
        (*list)[*to]["tiles"] = tile_rects(sets[*to]);
        id = (*list)[*to].value("id", std::string());
    }
    state.zoneRenames.erase((*list)[zone].value("id", std::string()));
    list->erase(list->begin() + std::ptrdiff_t(zone));
    return id;
}

std::string zone_scene_id(const MapState& state, const std::string& zone) {
    const auto it = state.zoneRenames.find(zone);
    return it == state.zoneRenames.end() ? zone : it->second;
}

void zones_add_tile(MapState& state, TileKey t) {
    Json* list = zones_json(state);
    if (!list || list->empty()) return;
    std::vector<TileSet> sets;
    for (const Json& z : *list) sets.push_back(zone_tiles(z));
    const auto neighbour = [&]() -> size_t {
        for (const TileKey d : kZoneNeighbours)
            for (size_t i = 0; i < sets.size(); ++i)
                if (sets[i].contains(offset(t, d))) return i;
        return 0; // no zoned neighbour: the first zone
    };
    const size_t to = neighbour();
    sets[to].insert(t);
    (*list)[to]["tiles"] = tile_rects(sets[to]);
}

void zones_remove_tile(MapState& state, TileKey t) {
    Json* list = zones_json(state);
    for (size_t i = 0; list && i < list->size(); ++i)
        if (TileSet tiles = zone_tiles((*list)[i]); tiles.erase(t)) (*list)[i]["tiles"] = tile_rects(tiles);
}

} // namespace mapedit
