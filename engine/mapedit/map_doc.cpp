#include "map_doc.h"

#include <log.h>

#include <format>
#include <set>

namespace mapedit {

bool same(const MapState& a, const MapState& b) {
    return a.manifest == b.manifest && a.areas == b.areas && a.spawns == b.spawns && a.objects == b.objects &&
           a.interactables == b.interactables && a.heights == b.heights && a.zones == b.zones &&
           a.zoneRenames == b.zoneRenames;
}

std::string tile_name(TileKey t) { return std::to_string(t.x) + "_" + std::to_string(t.z); }

std::optional<kast::map::MapInfo> MapDoc::info(std::string& err) const {
    return kast::map::parse_manifest(nlohmann::json(state.manifest), err);
}

namespace {

// Per tile: its value, or nullopt for no entry.
template <typename T>
using TileChanges = TileMap<std::optional<T>>;

// The parts of a MapState one edit changed, as they were before it or after it.
struct MapPart {
    std::optional<Json> manifest, areas, zones;
    std::optional<std::map<std::string, std::string>> zoneRenames;
    TileChanges<Json> spawns, objects, interactables;
    TileChanges<HeightsPtr> heights;
};

template <typename T>
std::optional<T> entry(const TileMap<T>& tiles, TileKey t) {
    const auto it = tiles.find(t);
    return it == tiles.end() ? std::nullopt : std::optional<T>(it->second);
}

template <typename T>
void diff(const TileMap<T>& a, const TileMap<T>& b, TileChanges<T>& before, TileChanges<T>& after) {
    std::set<TileKey, TileLess> keys;
    for (const auto& [t, v] : a) keys.insert(t);
    for (const auto& [t, v] : b) keys.insert(t);
    for (TileKey t : keys)
        if (auto va = entry(a, t), vb = entry(b, t); va != vb) {
            before[t] = std::move(va);
            after[t] = std::move(vb);
        }
}

template <typename T>
void apply(const TileChanges<T>& changes, TileMap<T>& tiles) {
    for (const auto& [t, v] : changes)
        if (v) tiles[t] = *v;
        else tiles.erase(t);
}

void apply(const MapPart& part, MapState& state) {
    if (part.manifest) state.manifest = *part.manifest;
    if (part.areas) state.areas = *part.areas;
    if (part.zones) state.zones = *part.zones;
    if (part.zoneRenames) state.zoneRenames = *part.zoneRenames;
    apply(part.spawns, state.spawns);
    apply(part.objects, state.objects);
    apply(part.interactables, state.interactables);
    apply(part.heights, state.heights);
}

// The step's name, for the part that changed (the first of these if several did).
std::string step_name(const MapPart& changed) {
    if (!changed.heights.empty()) return "Edit Terrain";
    if (changed.manifest) return "Edit Map";
    if (changed.zones || changed.zoneRenames) return "Edit Zones";
    if (!changed.objects.empty() || !changed.interactables.empty()) return "Edit Objects";
    if (!changed.spawns.empty()) return "Edit Spawns";
    return "Edit Areas";
}

} // namespace

void MapDoc::begin() { pending_ = state; }

void MapDoc::commit() {
    if (!pending_) return;
    const MapState before = std::move(*pending_);
    pending_.reset();
    auto parts = std::make_shared<std::pair<MapPart, MapPart>>(); // before, after
    auto& [was, now] = *parts;
    if (before.manifest != state.manifest) {
        was.manifest = before.manifest;
        now.manifest = state.manifest;
    }
    if (before.areas != state.areas) {
        was.areas = before.areas;
        now.areas = state.areas;
    }
    if (before.zones != state.zones) {
        was.zones = before.zones;
        now.zones = state.zones;
    }
    if (before.zoneRenames != state.zoneRenames) {
        was.zoneRenames = before.zoneRenames;
        now.zoneRenames = state.zoneRenames;
    }
    diff(before.spawns, state.spawns, was.spawns, now.spawns);
    diff(before.objects, state.objects, was.objects, now.objects);
    diff(before.interactables, state.interactables, was.interactables, now.interactables);
    diff(before.heights, state.heights, was.heights, now.heights);
    if (!now.manifest && !now.areas && !now.zones && !now.zoneRenames && now.spawns.empty() && now.objects.empty() && now.interactables.empty() &&
        now.heights.empty())
        return; // changed nothing
    history_.stack.push({step_name(now), [this, parts] { apply(parts->first, state); },
                         [this, parts] { apply(parts->second, state); }});
}

void MapDoc::undo() {
    if (editing() || !canUndo()) return;
    kast::log::info(std::format("Undo {}", history_.stack.undoName()));
    history_.stack.undo();
}

void MapDoc::redo() {
    if (editing() || !canRedo()) return;
    kast::log::info(std::format("Redo {}", history_.stack.redoName()));
    history_.stack.redo();
}

} // namespace mapedit
