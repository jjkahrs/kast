#include "zone_io.h"

#include "terrain_edit.h"

#include <assets.h> // recycleAsset
#include <files.h>
#include <objects.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <set>
#include <tuple>

namespace zone {
namespace {

namespace fs = std::filesystem;
namespace km = kast::map;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string where(const std::string& folder, const std::string& file) { return "maps/" + folder + "/" + file; }

// The JSON in file: nullopt and absent if there is none; nullopt with err set if it is not valid JSON.
std::optional<Json> read_json(const fs::path& file, bool& exists, std::string& err) {
    std::string text;
    exists = kast::readFile(file, text);
    if (!exists) return std::nullopt;
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        err = "not valid JSON";
        return std::nullopt;
    }
    return j;
}

// "<tx>_<tz>.json" -> the tile, as the server names spawn files.
std::optional<TileKey> spawn_file_tile(const std::string& name) {
    static const std::regex re(R"((-?\d{1,9})_(-?\d{1,9})\.json)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return std::nullopt;
    return TileKey{std::stoi(m[1]), std::stoi(m[2])};
}

template <typename Fn>
void for_each_tile(const km::MapInfo& info, Fn&& fn) {
    std::set<TileKey, TileLess> seen; // rects may overlap
    for (const km::TileRect& r : info.tiles)
        for (int32_t z = r.z0; z <= r.z1; ++z)
            for (int32_t x = r.x0; x <= r.x1; ++x)
                if (seen.insert({x, z}).second) fn(TileKey{x, z});
}

std::string dump(const Json& j) { return j.dump(2) + "\n"; }

} // namespace

std::string encode_r16(const km::Heights& heights) {
    std::string bytes(heights.size() * 2, '\0');
    for (size_t i = 0; i < heights.size(); ++i) {
        bytes[2 * i] = char(heights[i] & 0xFF);
        bytes[2 * i + 1] = char(heights[i] >> 8);
    }
    return bytes;
}

std::vector<std::string> npc_names(const fs::path& root) {
    static const std::regex nameField(R"(\bname\s*=\s*["']([^"']+)["'])");
    std::set<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root / "npcs", ec)) {
        std::string text;
        std::smatch m;
        if (entry.path().extension() == ".lua" && kast::readFile(entry.path(), text) && std::regex_search(text, m, nameField))
            names.insert(m[1]);
    }
    return {names.begin(), names.end()};
}

Project scan_project(const fs::path& root) {
    Project project;
    project.root = root;
    std::error_code ec;
    std::vector<fs::path> folders;
    for (const auto& entry : fs::directory_iterator(root / "maps", ec))
        if (entry.is_directory() && !entry.path().filename().string().starts_with('.') && fs::exists(entry.path() / "map.json"))
            folders.push_back(entry.path());
    std::sort(folders.begin(), folders.end());
    for (const fs::path& dir : folders) {
        const std::string folder = dir.filename().string();
        bool exists = false;
        std::string err;
        const auto manifest = read_json(dir / "map.json", exists, err);
        auto info = manifest ? km::parse_manifest(nlohmann::json(*manifest), err) : std::nullopt;
        if (!info) {
            project.errors.push_back({where(folder, "map.json"), err});
            continue;
        }
        if (const auto areas = read_json(dir / "areas.json", exists, err)) {
            if (auto parsed = km::parse_areas(nlohmann::json(*areas), err)) info->areas = std::move(*parsed);
            else project.errors.push_back({where(folder, "areas.json"), err});
        } else if (exists) {
            project.errors.push_back({where(folder, "areas.json"), err});
        }
        project.maps.push_back({folder, std::move(*info)});
    }
    return project;
}

uint16_t next_map_id(const Project& project) {
    std::set<uint16_t> used;
    for (const ProjectMap& m : project.maps) used.insert(m.info.id);
    uint16_t id = 0;
    while (used.contains(id) && id < 0xFFFF) ++id;
    return id;
}

std::optional<ZoneDoc> load_map(const Project& project, const std::string& folder, std::string& err) {
    const fs::path dir = project.root / "maps" / u8path(folder);
    ZoneDoc doc;
    doc.folder = folder;
    bool exists = false;
    const auto fail = [&](const std::string& file, const std::string& message) -> std::optional<ZoneDoc> {
        err = where(folder, file) + ": " + message;
        return std::nullopt;
    };

    auto manifest = read_json(dir / "map.json", exists, err);
    if (!manifest) return fail("map.json", exists ? err : "missing");
    doc.state.manifest = std::move(*manifest);
    const auto info = doc.info(err);
    if (!info) return fail("map.json", err);

    if (auto areas = read_json(dir / "areas.json", exists, err)) doc.state.areas = std::move(*areas);
    else if (exists) return fail("areas.json", err);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir / ".server" / "spawns", ec)) {
        const std::string name = entry.path().filename().string();
        const auto tile = spawn_file_tile(name);
        if (!tile) continue; // not ours to touch; validate() never sees it, the server reports it
        auto spawns = read_json(entry.path(), exists, err);
        if (!spawns) return fail(".server/spawns/" + name, err);
        doc.state.spawns[*tile] = std::move(*spawns);
    }
    for (const auto& [sub, files] : {std::pair{"objects", &doc.state.objects}, {".server/interactables", &doc.state.interactables}})
        for (const auto& entry : fs::directory_iterator(dir / sub, ec)) {
            const std::string name = entry.path().filename().string();
            const auto tile = spawn_file_tile(name);
            if (!tile) continue; // as spawn files
            auto json = read_json(entry.path(), exists, err);
            if (!json) return fail(std::string(sub) + "/" + name, err);
            (*files)[*tile] = std::move(*json);
        }

    bool ok = true;
    for_each_tile(*info, [&](TileKey t) {
        for (const char* ext : {".r16", ".png"}) {
            const std::string name = tile_name(t) + ext;
            std::string bytes;
            if (!ok || !kast::readFile(dir / "heights" / name, bytes)) continue;
            auto heights = km::decode_heights(bytes, ext, err);
            if (!heights) {
                err = where(folder, "heights/" + name) + ": " + err;
                ok = false;
                return;
            }
            doc.state.heights[t] = std::make_shared<const km::Heights>(std::move(*heights));
            doc.heightExt[t] = ext;
            break;
        }
    });
    if (!ok) return std::nullopt;
    doc.saved = doc.state;
    return doc;
}

std::optional<ZoneDoc> new_map(const Project& project, const std::string& folder, const std::string& name, uint16_t id,
                               int width, int depth, std::string& err) {
    static const std::regex plain(R"([A-Za-z0-9_][A-Za-z0-9_-]*)");
    if (!std::regex_match(folder, plain)) {
        err = "folder \"" + folder + "\": use letters, digits, _ and - (not starting with -)";
        return std::nullopt;
    }
    if (fs::exists(project.root / "maps" / u8path(folder))) {
        err = "maps/" + folder + " already exists";
        return std::nullopt;
    }
    if (width < 1 || depth < 1 || width > 8 || depth > 8) {
        err = "tiles: 1 to 8 each way";
        return std::nullopt;
    }
    ZoneDoc doc;
    doc.folder = folder;
    doc.state.manifest = {{"version", 1},
                          {"id", id},
                          {"name", name},
                          {"tiles", Json::array({Json::array({0, 0, width - 1, depth - 1})})},
                          {"heightRange", Json::array({0, 100})},
                          {"spawn", {{"pos", Json::array({128, 0, 128})}, {"heading", 0}}}};
    return doc;
}

std::vector<ZoneError> validate(const Project& project, const ZoneDoc& doc) {
    std::vector<ZoneError> errors;
    std::string err;
    const auto info = doc.info(err);
    if (!info) return {{where(doc.folder, "map.json"), err}};

    for (const ProjectMap& m : project.maps)
        if (m.folder != doc.folder && m.info.id == info->id)
            errors.push_back({where(doc.folder, "map.json"), "id " + std::to_string(info->id) + " already used by maps/" + m.folder});
    if (doc.state.manifest.contains("terrainColors") && !parse_gradient(doc.state.manifest, err))
        errors.push_back({where(doc.folder, "map.json"), err});
    // Save overwrites the generated terrain files; until the map has been saved pointing at them, a file already there
    // is someone else's.
    const std::string generated = terrain_material_path(doc.folder);
    if (info->material == generated && !(doc.saved.manifest.is_object() && doc.saved.manifest.value("material", "") == generated))
        for (const std::string& file : {generated, terrain_texture_path(doc.folder)})
            if (fs::exists(project.root / u8path(file)))
                errors.push_back({file, "already exists and was not generated for this map; rename or delete it"});

    if (!doc.state.areas.is_null()) {
        if (const auto areas = km::parse_areas(nlohmann::json(doc.state.areas), err)) {
            // Portals as the server checks them once every map is loaded (maps.cpp portal_error).
            for (const km::Area& area : areas->list) {
                if (!area.portal) continue;
                const km::Portal& p = *area.portal;
                const std::string at = "area \"" + area.id + "\": portal ";
                const km::MapInfo* to = p.map == info->id ? &*info : nullptr;
                const km::Areas* toAreas = to ? &*areas : nullptr;
                for (const ProjectMap& m : project.maps)
                    if (!to && m.folder != doc.folder && m.info.id == p.map) {
                        to = &m.info;
                        toAreas = &m.info.areas;
                    }
                if (!to) errors.push_back({where(doc.folder, "areas.json"), at + "to unknown map " + std::to_string(p.map)});
                else if (!to->contains(p.pos[0], p.pos[2]))
                    errors.push_back({where(doc.folder, "areas.json"), at + "destination is not on a tile of map " + std::to_string(p.map)});
                else if (const int16_t i = km::area_at(*toAreas, p.pos[0], p.pos[1], p.pos[2]); i >= 0 && toAreas->list[i].portal)
                    errors.push_back({where(doc.folder, "areas.json"), at + "destination is inside portal area \"" +
                                                                           toAreas->list[i].id + "\" of map " + std::to_string(p.map)});
            }
        } else {
            errors.push_back({where(doc.folder, "areas.json"), err});
        }
    }

    std::set<std::string> ids;
    for (const auto& [tile, json] : doc.state.spawns) {
        const std::string file = where(doc.folder, ".server/spawns/" + tile_name(tile) + ".json");
        if (!info->has_tile(tile)) {
            errors.push_back({file, "tile is not on the map"});
            continue;
        }
        const auto spawns = km::parse_spawns(nlohmann::json(json), tile, err);
        if (!spawns) {
            errors.push_back({file, err});
            continue;
        }
        for (const km::Spawn& s : *spawns)
            if (!ids.insert(s.id).second) errors.push_back({file, "spawn \"" + s.id + "\": duplicate id on this map"});
    }

    // Objects as kast_mapbake lints them, the server's interactables as the server loads them, and the two in step: each
    // interactable object has a server entry of the same id in the same tile's file, and each entry an object.
    std::set<uint32_t> objectIds, serverIds;
    TileMap<std::set<uint32_t>> objectsByTile, serverByTile;
    for (const auto& [tile, json] : doc.state.objects) {
        const std::string file = where(doc.folder, "objects/" + tile_name(tile) + ".json");
        if (!info->has_tile(tile)) {
            errors.push_back({file, "tile is not on the map"});
            continue;
        }
        const km::ParsedObjects parsed = km::parse_objects(nlohmann::json(json), tile);
        for (const std::string& e : parsed.errors) errors.push_back({file, e});
        for (const km::ZoneObject& o : parsed.objects) {
            if (o.type != km::ObjectType::Interactable) continue;
            if (!objectIds.insert(o.id).second)
                errors.push_back({file, "interactable " + std::to_string(o.id) + ": duplicate id on this map"});
            objectsByTile[tile].insert(o.id);
        }
    }
    for (const auto& [tile, json] : doc.state.interactables) {
        const std::string file = where(doc.folder, ".server/interactables/" + tile_name(tile) + ".json");
        if (!info->has_tile(tile)) {
            errors.push_back({file, "tile is not on the map"});
            continue;
        }
        const auto list = km::parse_interactables(nlohmann::json(json), tile, err);
        if (!list) {
            errors.push_back({file, err});
            continue;
        }
        for (const km::Interactable& it : *list) {
            if (!serverIds.insert(it.id).second)
                errors.push_back({file, "interactable " + std::to_string(it.id) + ": duplicate id on this map"});
            serverByTile[tile].insert(it.id);
        }
    }
    for (const auto& [tile, tileIds] : objectsByTile)
        for (const uint32_t id : tileIds)
            if (!serverByTile[tile].contains(id))
                errors.push_back({where(doc.folder, "objects/" + tile_name(tile) + ".json"),
                                  "interactable " + std::to_string(id) + " has no entry in .server/interactables/" + tile_name(tile) + ".json"});
    for (const auto& [tile, tileIds] : serverByTile)
        for (const uint32_t id : tileIds)
            if (!objectsByTile[tile].contains(id))
                errors.push_back({where(doc.folder, ".server/interactables/" + tile_name(tile) + ".json"),
                                  "interactable " + std::to_string(id) + " has no object in objects/" + tile_name(tile) + ".json"});
    return errors;
}

std::vector<std::string> tile_files(const Project& project, const std::string& folder, TileKey t) {
    const std::string name = tile_name(t);
    std::vector<std::string> files;
    for (const std::string file : {"heights/" + name + ".r16", "heights/" + name + ".png", ".server/spawns/" + name + ".json",
                                   ".server/nav/" + name + ".navtile", "objects/" + name + ".json",
                                   ".server/interactables/" + name + ".json"})
        if (fs::exists(project.root / "maps" / u8path(folder) / u8path(file))) files.push_back(where(folder, file));
    return files;
}

std::vector<std::string> removed_tile_files(const Project& project, const ZoneDoc& doc) {
    std::string err;
    const auto before = doc.saved.manifest.is_null() ? std::nullopt : km::parse_manifest(nlohmann::json(doc.saved.manifest), err);
    const auto now = doc.info(err);
    if (!before || !now) return {};
    std::vector<std::string> files;
    for_each_tile(*before, [&](TileKey t) {
        if (now->has_tile(t)) return;
        for (std::string& file : tile_files(project, doc.folder, t)) files.push_back(std::move(file));
    });
    return files;
}

std::vector<ZoneError> save_map(Project& project, ZoneDoc& doc, bool recycle) {
    std::vector<ZoneError> errors = validate(project, doc);
    if (!errors.empty()) return errors;
    std::vector<std::string> removed = removed_tile_files(project, doc);
    // A tile still declared that lost its heights (removed and added back: flat again) or its spawn file: its old files
    // would be read back, so they go too.
    const auto drop = [&](const std::string& file) {
        if (fs::exists(project.root / u8path(file)) && std::find(removed.begin(), removed.end(), file) == removed.end())
            removed.push_back(file);
    };
    for (const auto& [tile, heights] : doc.saved.heights)
        if (!doc.state.heights.contains(tile))
            for (const char* ext : {".r16", ".png"}) drop(where(doc.folder, "heights/" + tile_name(tile) + ext));
    for (const auto& [tile, json] : doc.saved.spawns)
        if (!doc.state.spawns.contains(tile)) drop(where(doc.folder, ".server/spawns/" + tile_name(tile) + ".json"));
    for (const auto& [tile, json] : doc.saved.objects)
        if (!doc.state.objects.contains(tile)) drop(where(doc.folder, "objects/" + tile_name(tile) + ".json"));
    for (const auto& [tile, json] : doc.saved.interactables)
        if (!doc.state.interactables.contains(tile)) drop(where(doc.folder, ".server/interactables/" + tile_name(tile) + ".json"));
    const fs::path dir = project.root / "maps" / u8path(doc.folder);
    const auto write = [&](const std::string& file, std::string_view bytes) {
        const fs::path path = dir / u8path(file);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (!kast::writeFileAtomic(path, bytes)) errors.push_back({where(doc.folder, file), "cannot write"});
    };
    const auto remove = [&](const std::string& file) { // project-relative
        const fs::path path = project.root / u8path(file);
        std::string err;
        std::error_code ec;
        if (recycle ? !recycleAsset(path, err) : !fs::remove(path, ec)) errors.push_back({file, recycle ? err : ec.message()});
    };

    // The generated terrain texture and material follow the heights and colors on every save, once Create assigned
    // them; baked before anything is written.
    std::string terrainPng, terrainMat;
    std::string err;
    if (const auto info = doc.info(err); info && info->material == terrain_material_path(doc.folder))
        if (const auto gradient = parse_gradient(doc.state.manifest, err)) {
            terrainPng = encode_png_rgb(bake_terrain(*info, doc.state, *gradient));
            terrainMat = kast::materialToJson(terrain_material(terrain_texture_path(doc.folder))).dump(2) + "\n";
            if (terrainPng.empty()) return {{terrain_texture_path(doc.folder), "cannot encode the PNG"}};
        }
    const auto writeProject = [&](const std::string& file, std::string_view bytes) { // project-relative
        const fs::path path = project.root / u8path(file);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (!kast::writeFileAtomic(path, bytes)) errors.push_back({file, "cannot write"});
    };

    if (!terrainPng.empty()) {
        writeProject(terrain_texture_path(doc.folder), terrainPng);
        writeProject(terrain_material_path(doc.folder), terrainMat);
    }
    if (doc.state.manifest != doc.saved.manifest) write("map.json", dump(doc.state.manifest));
    if (!doc.state.areas.is_null() && doc.state.areas != doc.saved.areas) write("areas.json", dump(doc.state.areas));
    for (const auto& [tile, json] : doc.state.spawns) {
        const auto old = doc.saved.spawns.find(tile);
        if (old == doc.saved.spawns.end() || old->second != json) write(".server/spawns/" + tile_name(tile) + ".json", dump(json));
    }
    for (const auto& [sub, files, saved] : {std::tuple{"objects/", &doc.state.objects, &doc.saved.objects},
                                            {".server/interactables/", &doc.state.interactables, &doc.saved.interactables}})
        for (const auto& [tile, json] : *files) {
            const auto old = saved->find(tile);
            if (old == saved->end() || old->second != json) write(sub + tile_name(tile) + ".json", dump(json));
        }
    for (const auto& [tile, heights] : doc.state.heights) {
        const auto old = doc.saved.heights.find(tile);
        if (old != doc.saved.heights.end() && old->second == heights) continue;
        write("heights/" + tile_name(tile) + ".r16", encode_r16(*heights));
        // The loaders read .r16 first, so the old .png would only be stale data.
        if (doc.heightExt[tile] == ".png") remove(where(doc.folder, "heights/" + tile_name(tile) + ".png"));
        doc.heightExt[tile] = ".r16";
    }
    for (const std::string& file : removed) remove(file);
    for (auto it = doc.heightExt.begin(); it != doc.heightExt.end();)
        it = doc.state.heights.contains(it->first) ? std::next(it) : doc.heightExt.erase(it);
    if (!errors.empty()) return errors;
    doc.saved = doc.state;
    project = scan_project(project.root);
    return {};
}

} // namespace zone
