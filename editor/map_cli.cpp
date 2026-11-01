// The map edit flags (map_cli.h): parsing each one and running it on the MapSession.
#include "map_cli.h"

#include "map_panel.h"

#include <files.h>
#include <log.h>
#include <scene_io.h> // jsonNumber

#include <bx/math.h>

#include <mapedit/terrain_edit.h>
#include <mapedit/map_edit.h>
#include <mapedit/zone_edit.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <thread>

namespace map_cli {
namespace {

namespace fs = std::filesystem;
using mapedit::Json;

// Flags that are edits taking a value, run in command-line order.
constexpr std::string_view kValueOps[] = {"--map", "--new-map", "--falloff", "--sculpt", "--flatten", "--height-range",
                                          "--import-png", "--export-png", "--material", "--generate-heights",
                                          "--terrain-colors", "--add-area", "--add-polygon", "--area-parent",
                                          "--area-portal", "--move-area-vertex", "--delete-area", "--add-spawn",
                                          "--move-spawn", "--delete-spawn", "--add-path", "--map-id", "--add-tile",
                                          "--remove-tile", "--set-spawn", "--add-object", "--set-interactable",
                                          "--move-object", "--delete-object", "--scatter", "--erase-visuals",
                                          "--instance", "--add-zone", "--paint-zone", "--delete-zone", "--rename-zone",
                                          "--zone-name", "--zone-flags"};
constexpr std::string_view kFlagOps[] = {"--save-map", "--expect-save-fail", "--undo", "--redo", "--validate",
                                         "--create-terrain-material", "--world-map"};
constexpr float kLoopRadius = 24.0f; // --sculpt-loop's brush

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::vector<std::string> split(std::string_view text, char sep)
{
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t end = text.find(sep, start);
        parts.emplace_back(text.substr(start, end - start));
        if (end == std::string_view::npos)
            return parts;
        start = end + 1;
    }
}

bool parseInt(std::string_view text, int& out)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

bool parseFloat(std::string_view text, float& out)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size() && std::isfinite(out);
}

// "a,b,c": exactly min..max finite numbers.
std::optional<std::vector<float>> parseFloats(std::string_view text, size_t min, size_t max)
{
    std::vector<float> out;
    for (const std::string& part : split(text, ','))
        if (!parseFloat(part, out.emplace_back()))
            return std::nullopt;
    if (out.size() < min || out.size() > max)
        return std::nullopt;
    return out;
}

// "tx,tz,file": the file may itself contain commas.
std::optional<std::pair<mapedit::TileKey, fs::path>> parseTileFile(std::string_view text)
{
    const auto parts = split(text, ',');
    mapedit::TileKey t;
    if (parts.size() < 3 || !parseInt(parts[0], t.x) || !parseInt(parts[1], t.z))
        return std::nullopt;
    const std::string_view file = text.substr(parts[0].size() + parts[1].size() + 2);
    return std::pair{t, fs::path(std::u8string(file.begin(), file.end()))};
}

// The terrain edits (the old Session's), one undo step each.
bool sculpt(mapedit::MapDoc& doc, const mapedit::Brush& brush, float x, float z, float amount)
{
    std::string err;
    const auto info = doc.info(err);
    if (!info)
        return false;
    mapedit::TileSet touched;
    doc.edit([&](mapedit::MapState& s) { touched = mapedit::apply_brush(*info, s, x, z, brush, amount); });
    return !touched.empty();
}

bool setHeightRange(mapedit::MapDoc& doc, float min, float max)
{
    std::string err;
    const auto info = doc.info(err);
    if (!info || !(min <= max)) {
        kast::log::error("Map: height range: min must be <= max");
        return false;
    }
    size_t clipped = 0;
    doc.edit([&](mapedit::MapState& s) { clipped = mapedit::set_height_range(*info, s, min, max); });
    kast::log::info(std::format("Map: height range now {} .. {} m{}", min, max,
                                clipped ? std::format(" ({} samples clipped)", clipped) : ""));
    return true;
}

bool importPng(mapedit::MapDoc& doc, mapedit::TileKey tile, const fs::path& file)
{
    std::string err, bytes;
    const auto info = doc.info(err);
    if (!info || !info->has_tile(tile)) {
        kast::log::error("Map: import PNG: tile " + mapedit::tile_name(tile) + " is not on the map");
        return false;
    }
    bool ok = kast::readFile(file, bytes);
    if (!ok)
        err = "cannot read the file";
    else
        doc.edit([&](mapedit::MapState& s) { ok = mapedit::import_png16(*info, s, tile, bytes, err); });
    if (ok)
        kast::log::info(std::format("Map: imported {} into tile {}", utf8(file), mapedit::tile_name(tile)));
    else
        kast::log::error(std::format("Map: import PNG {}: {}", utf8(file), err));
    return ok;
}

bool exportPng(const mapedit::MapDoc& doc, mapedit::TileKey tile, const fs::path& file)
{
    std::string err;
    const auto info = doc.info(err);
    if (!info || !info->has_tile(tile)) {
        kast::log::error("Map: export PNG: tile " + mapedit::tile_name(tile) + " is not on the map");
        return false;
    }
    const bool ok = kast::writeFileAtomic(file, mapedit::encode_png16(mapedit::tile_heights(doc.state, tile)));
    if (ok)
        kast::log::info(std::format("Map: exported tile {} to {}", mapedit::tile_name(tile), utf8(file)));
    else
        kast::log::error("Map: export PNG: cannot write " + utf8(file));
    return ok;
}

bool generateHeights(mapedit::MapDoc& doc, const mapedit::NoiseParams& params)
{
    std::string err;
    const auto info = doc.info(err);
    if (!info)
        return false;
    doc.edit([&](mapedit::MapState& s) {
        mapedit::generate_heights(*info, s, params);
        // The spawn stays on the ground (it is on the map: the manifest parsed).
        s.manifest["spawn"]["pos"][1] = kast::jsonNumber(*mapedit::ground_y(*info, s, info->spawn[0], info->spawn[2]));
    });
    kast::log::info(std::format("Map: generated heights (seed {}, scale {} m, {} octaves)", params.seed, params.scale,
                                params.octaves));
    return true;
}

void createTerrainMaterial(mapedit::MapDoc& doc)
{
    std::string err;
    const bool hasColors = mapedit::parse_gradient(doc.state.manifest, err).has_value();
    doc.edit([&](mapedit::MapState& s) {
        if (!hasColors)
            s.manifest["terrainColors"] = mapedit::gradient_json(mapedit::default_gradient());
        s.manifest["terrainUv"] = "map";
        s.manifest["material"] = mapedit::terrain_material_path(doc.folder);
    });
    kast::log::info("Map: terrain material " + mapedit::terrain_material_path(doc.folder) + " and " +
                    mapedit::terrain_texture_path(doc.folder) + " assigned; written on save");
}

// The area, spawn, path and object edits (one undo step each); nullopt if op is not one of them. Positions snap to the
// ground.
std::optional<bool> objectEdit(mapedit::MapDoc& doc, const Op& op)
{
    const auto parts = split(op.value, ',');
    const auto bad = [&](const char* form) -> std::optional<bool> {
        kast::log::error(std::format("{} {}: expected {}", op.flag, op.value, form));
        return false;
    };
    // parts[from..] as floats, or nullopt.
    const auto floats = [&](size_t from) -> std::optional<std::vector<float>> {
        std::vector<float> out;
        for (size_t i = from; i < parts.size(); ++i)
            if (!parseFloat(parts[i], out.emplace_back()))
                return std::nullopt;
        return out;
    };
    std::string err;
    const auto info = doc.info(err);
    const auto ground = [&](float x, float z) {
        return mapedit::Vec3{x, info ? mapedit::ground_y(*info, doc.state, x, z).value_or(0.0f) : 0.0f, z};
    };
    const auto area = [&](const std::string& id) { return mapedit::find_area(doc.state, id); };
    const auto spawn = [&](const std::string& id) { return mapedit::find_spawn(doc.state, id); };
    int priority = 0;

    if (op.flag == "--add-area") {
        const auto v = parts.size() == 6 && parseInt(parts[1], priority) ? floats(2) : std::nullopt;
        if (!v)
            return bad("id,priority,x0,z0,x1,z1");
        doc.edit([&](mapedit::MapState& s) { mapedit::add_rect_area(s, parts[0], {(*v)[0], (*v)[1]}, {(*v)[2], (*v)[3]}, priority); });
        return true;
    }
    if (op.flag == "--add-polygon") {
        const auto v = parts.size() >= 8 && parts.size() % 2 == 0 && parseInt(parts[1], priority) ? floats(2) : std::nullopt;
        if (!v)
            return bad("id,priority,x,z,x,z,x,z[,...]");
        std::vector<mapedit::Point> points;
        for (size_t i = 0; i < v->size(); i += 2)
            points.push_back({(*v)[i], (*v)[i + 1]});
        doc.edit([&](mapedit::MapState& s) { mapedit::add_polygon_area(s, parts[0], points, priority); });
        return true;
    }
    if (op.flag == "--area-parent") {
        if (parts.size() != 2 || !area(parts[0]))
            return bad("id,parent (an existing area)");
        doc.edit([&](mapedit::MapState& s) { mapedit::areas_list(s)[*area(parts[0])]["parent"] = parts[1]; });
        return true;
    }
    if (op.flag == "--area-portal") {
        int map = 0;
        const auto v = parts.size() == 6 && parseInt(parts[1], map) && map >= 0 && map <= 0xFFFF ? floats(2) : std::nullopt;
        if (!v || !area(parts[0]))
            return bad("id,map,x,y,z,heading (an existing area)");
        doc.edit([&](mapedit::MapState& s) {
            mapedit::areas_list(s)[*area(parts[0])]["portal"] = {{"map", unsigned(map)},
                                                              {"pos", mapedit::vec3_json({(*v)[0], (*v)[1], (*v)[2]})},
                                                              {"heading", kast::jsonNumber((*v)[3])}};
        });
        return true;
    }
    if (op.flag == "--move-area-vertex") {
        int index = 0;
        const auto v = parts.size() == 4 && parseInt(parts[1], index) && index >= 0 ? floats(2) : std::nullopt;
        if (!v || !area(parts[0]))
            return bad("id,vertex,x,z (an existing area)");
        doc.edit([&](mapedit::MapState& s) {
            mapedit::move_area_vertex(mapedit::areas_list(s)[*area(parts[0])], size_t(index), {(*v)[0], (*v)[1]});
        });
        return true;
    }
    if (op.flag == "--delete-area") {
        if (!area(op.value))
            return bad("id (an existing area)");
        doc.edit([&](mapedit::MapState& s) { mapedit::delete_area(s, *area(op.value)); });
        return true;
    }
    if (op.flag == "--add-spawn") {
        const auto v = parts.size() >= 4 && parts.size() <= 6 ? floats(2) : std::nullopt;
        if (!v)
            return bad("id,npc,x,z[,heading[,respawnSec]]");
        doc.edit([&](mapedit::MapState& s) {
            mapedit::add_spawn(s, parts[0], parts[1], ground((*v)[0], (*v)[1]), v->size() > 2 ? (*v)[2] : 0.0f,
                            v->size() > 3 ? (*v)[3] : 60.0f);
        });
        return true;
    }
    // Map objects: addressed by tile and index in its objects file ("tx,tz,i"), interactables by id.
    const auto objectAt = [&](size_t from) -> std::optional<mapedit::ObjectRef> {
        int tx = 0, tz = 0, index = 0;
        if (parts.size() < from + 3 || !parseInt(parts[from], tx) || !parseInt(parts[from + 1], tz) ||
            !parseInt(parts[from + 2], index))
            return std::nullopt;
        const mapedit::ObjectRef ref{{tx, tz}, size_t(index)};
        const auto refs = mapedit::all_objects(doc.state);
        return std::find(refs.begin(), refs.end(), ref) != refs.end() ? std::optional(ref) : std::nullopt;
    };
    if (op.flag == "--add-object") {
        const std::string form = "type,prefab,x,z[,yaw[,scale[,box,hx,hy,hz | cylinder,r,h]]]";
        const std::vector<std::string> types = {"visual", "obstacle", "interactable"};
        const auto type = parts.empty() ? types.end() : std::find(types.begin(), types.end(), parts[0]);
        const size_t shapeAt = std::min<size_t>(parts.size(), 6);
        std::vector<float> v;
        for (size_t i = 2; i < shapeAt; ++i)
            if (!parseFloat(parts[i], v.emplace_back()))
                return bad(form.c_str());
        const auto shape = parts.size() > 6 ? floats(7) : std::optional<std::vector<float>>(std::vector<float>{});
        const bool box = parts.size() > 6 && parts[6] == "box" && shape && shape->size() == 3;
        const bool cylinder = parts.size() > 6 && parts[6] == "cylinder" && shape && shape->size() == 2;
        if (type == types.end() || parts.size() < 4 || v.size() < 2 || (parts.size() > 6 && !box && !cylinder))
            return bad(form.c_str());
        doc.edit([&](mapedit::MapState& s) {
            const mapedit::ObjectRef ref = mapedit::add_object(s, kast::map::ObjectType(type - types.begin()), parts[1],
                                                         ground(v[0], v[1]), v.size() > 2 ? v[2] : 0.0f,
                                                         v.size() > 3 ? v[3] : 1.0f);
            Json& o = mapedit::object_json(s, ref);
            if (box)
                o["shape"] = {{"box", {{"halfExtents", mapedit::vec3_json({(*shape)[0], (*shape)[1], (*shape)[2]})}}}};
            if (cylinder)
                o["shape"] = {{"cylinder", {{"radius", kast::jsonNumber((*shape)[0])}, {"height", kast::jsonNumber((*shape)[1])}}}};
            if (o.value("type", "") == "interactable")
                mapedit::sync_interactable(s, ref);
        });
        return true;
    }
    if (op.flag == "--set-interactable") { // id,range,event[,name[,data]]: data is the rest, commas and all
        int id = 0;
        float range = 0;
        std::optional<mapedit::ObjectRef> ref;
        for (const mapedit::ObjectRef& r : mapedit::all_objects(doc.state))
            if (parts.size() >= 3 && parseInt(parts[0], id) && mapedit::object_json(doc.state, r).value("id", Json()) == id)
                ref = r;
        Json data;
        if (parts.size() > 4) {
            const size_t at = parts[0].size() + parts[1].size() + parts[2].size() + parts[3].size() + 4;
            data = Json::parse(op.value.substr(at), nullptr, false);
        }
        if (!ref || !parseFloat(parts[1], range) || (parts.size() > 4 && !data.is_object()))
            return bad("id,range,event[,name[,data JSON object]] (an existing interactable)");
        doc.edit([&](mapedit::MapState& s) {
            if (parts.size() > 3 && !parts[3].empty())
                mapedit::object_json(s, *ref)["name"] = parts[3];
            mapedit::sync_interactable(s, *ref);
            Json& entry = *mapedit::interactable_json(s, ref->tile, uint32_t(id));
            entry["range"] = kast::jsonNumber(range);
            entry["event"] = parts[2];
            if (data.is_object())
                entry["data"] = data;
        });
        return true;
    }
    if (op.flag == "--move-object") {
        const auto ref = objectAt(0);
        const auto v = ref && parts.size() == 5 ? floats(3) : std::nullopt;
        if (!v)
            return bad("tx,tz,index,x,z (an existing object)");
        doc.edit([&](mapedit::MapState& s) { mapedit::move_object(s, *ref, ground((*v)[0], (*v)[1])); });
        return true;
    }
    if (op.flag == "--delete-object") {
        const auto ref = objectAt(0);
        if (!ref || parts.size() != 3)
            return bad("tx,tz,index (an existing object)");
        doc.edit([&](mapedit::MapState& s) { mapedit::delete_object(s, *ref); });
        return true;
    }
    if (op.flag == "--scatter") { // one stroke of one dab, as the Scatter tool's
        mapedit::Scatter brush;
        int count = 0, seed = 0;
        std::vector<float> f; // x, z, radius, scaleMin, scaleMax
        for (const size_t i : {0, 1, 2, 5, 6})
            if (i < parts.size() && !parseFloat(parts[i], f.emplace_back()))
                f.pop_back();
        if (parts.size() != 8 || f.size() != 5 || !parseInt(parts[3], count) || !parseInt(parts[4], seed) || count < 1 || !info)
            return bad("x,z,radius,count,seed,scaleMin,scaleMax,prefab[+prefab...]");
        brush.radius = f[2];
        brush.count = count;
        brush.scaleMin = f[3];
        brush.scaleMax = f[4];
        brush.prefabs = split(parts[7], '+');
        mapedit::ScatterRng rng{uint32_t(seed)};
        size_t placed = 0;
        doc.edit([&](mapedit::MapState& s) { placed = mapedit::scatter(*info, s, brush, rng, f[0], f[1]); });
        kast::log::info(std::format("Map: --scatter: {} visual(s) placed", placed));
        return true;
    }
    if (op.flag == "--erase-visuals") {
        const auto v = parts.size() == 3 ? floats(0) : std::nullopt;
        if (!v)
            return bad("x,z,radius");
        size_t erased = 0;
        doc.edit([&](mapedit::MapState& s) { erased = mapedit::erase_visuals(s, (*v)[0], (*v)[1], (*v)[2]); });
        kast::log::info(std::format("Map: --erase-visuals: {} visual(s) removed", erased));
        return true;
    }
    if (op.flag == "--move-spawn") {
        const auto v = parts.size() == 3 ? floats(1) : std::nullopt;
        if (!v || !spawn(parts[0]))
            return bad("id,x,z (an existing spawn)");
        doc.edit([&](mapedit::MapState& s) { mapedit::move_spawn(s, *spawn(parts[0]), ground((*v)[0], (*v)[1])); });
        return true;
    }
    if (op.flag == "--delete-spawn") {
        if (!spawn(op.value))
            return bad("id (an existing spawn)");
        doc.edit([&](mapedit::MapState& s) { mapedit::delete_spawn(s, *spawn(op.value)); });
        return true;
    }
    if (op.flag == "--add-path") {
        int loop = 0;
        const auto v = parts.size() >= 5 && parts.size() % 2 == 1 && parseInt(parts[2], loop) ? floats(3) : std::nullopt;
        float wait = 0;
        if (!v || !parseFloat(parts[1], wait) || !spawn(parts[0]))
            return bad("spawnId,wait,loop(0|1),x,z[,x,z...]");
        std::vector<mapedit::Vec3> points;
        for (size_t i = 0; i < v->size(); i += 2)
            points.push_back(ground((*v)[i], (*v)[i + 1]));
        doc.edit([&](mapedit::MapState& s) { mapedit::add_path(s, *spawn(parts[0]), parts[0] + "_path", points, wait, loop != 0); });
        return true;
    }
    return std::nullopt;
}

// The zone edits (zone_edit.h), one undo step each; nullopt: op is not one. A zone is named by its id (the implicit
// zone's is the map folder).
std::optional<bool> zoneEdit(mapedit::MapDoc& doc, const Op& op)
{
    const auto bad = [&](std::string_view form) {
        kast::log::error(std::format("{} {}: expected {}", op.flag, op.value, form));
        return false;
    };
    std::string err;
    const auto info = doc.info(err);
    const auto parts = split(op.value, ',');
    // The zone named by parts[0] (made explicit first: the edit writes zones.json).
    const auto zone = [&]() -> std::optional<size_t> {
        if (!info || parts.empty())
            return std::nullopt;
        if (doc.state.zones.is_null() && parts[0] == doc.folder)
            return 0;
        return mapedit::find_zone(doc.state, parts[0]);
    };
    const auto explicitZones = [&](mapedit::MapState& s) { mapedit::zones_list(*info, s, doc.folder); };
    if (op.flag == "--add-zone") { // id[,name]
        if (!info || parts.empty() || parts.size() > 2 || !kast::map::valid_zone_id(parts[0]) || zone())
            return bad("id[,name] (a new id: letters, digits, _ and -)");
        doc.edit([&](mapedit::MapState& s) { mapedit::add_zone(*info, s, doc.folder, parts[0], parts.size() > 1 ? parts[1] : ""); });
        kast::log::info(std::format("Map: added zone {} (no tiles yet: paint some on the grid)", parts[0]));
        return true;
    }
    if (op.flag == "--paint-zone") { // id,tx,tz[,tx2,tz2]: the rect's declared tiles
        int v[4] = {};
        bool ok = (parts.size() == 3 || parts.size() == 5) && zone();
        for (size_t i = 1; ok && i < parts.size(); ++i)
            ok = parseInt(parts[i], v[i - 1]);
        if (!ok)
            return bad("id,tx,tz[,tx2,tz2] (an existing zone)");
        if (parts.size() == 3)
            v[2] = v[0], v[3] = v[1];
        mapedit::TileSet tiles;
        for (int tz = std::min(v[1], v[3]); tz <= std::max(v[1], v[3]); ++tz)
            for (int tx = std::min(v[0], v[2]); tx <= std::max(v[0], v[2]); ++tx)
                tiles.insert({tx, tz});
        size_t moved = 0;
        doc.edit([&](mapedit::MapState& s) {
            explicitZones(s);
            moved = mapedit::paint_zone(*info, s, *mapedit::find_zone(s, parts[0]), tiles);
        });
        kast::log::info(std::format("Map: painted {} tile(s) into zone {}", moved, parts[0]));
        return true;
    }
    if (op.flag == "--delete-zone" || op.flag == "--rename-zone") {
        const bool rename = op.flag == "--rename-zone";
        if (!zone() || parts.size() != (rename ? 2u : 1u) ||
            (rename && (!kast::map::valid_zone_id(parts[1]) || mapedit::find_zone(doc.state, parts[1]))))
            return bad(rename ? "old,new (an existing zone, a new valid id)" : "id (an existing zone)");
        if (rename)
            MapPanel::renameZone(doc, *info, parts[0], parts[1]);
        else
            MapPanel::deleteZone(doc, *info, parts[0]);
        return true;
    }
    if (op.flag == "--zone-name" || op.flag == "--zone-flags") { // id,name | id,{json}: the rest, commas and all
        const std::string rest = parts.size() > 1 ? op.value.substr(parts[0].size() + 1) : "";
        const Json flags = op.flag == "--zone-flags" ? Json::parse(rest, nullptr, false) : Json();
        if (!zone() || parts.size() < 2 || (op.flag == "--zone-flags" && !flags.is_object()))
            return bad(op.flag == "--zone-name" ? "id,name (an existing zone)" : "id,{JSON object} (an existing zone)");
        doc.edit([&](mapedit::MapState& s) {
            explicitZones(s);
            Json& z = mapedit::zones_list(*info, s, doc.folder)[*mapedit::find_zone(s, parts[0])];
            if (op.flag == "--zone-name")
                z["name"] = rest;
            else
                z["flags"] = flags;
        });
        kast::log::info(std::format("Map: zone {} {} {}", parts[0], op.flag == "--zone-name" ? "named" : "flags", rest));
        return true;
    }
    return std::nullopt;
}

// Runs one map edit; false (logged) stops the run. brush: --falloff's, for later sculpts.
bool runOp(MapSession& session, const Op& op, mapedit::Brush& brush)
{
    const auto bad = [&](std::string_view form) {
        kast::log::error(std::format("{} {}: expected {}", op.flag, op.value, form));
        return false;
    };
    if (op.flag == "--falloff") {
        if (!parseFloat(op.value, brush.falloff) || brush.falloff < 0 || brush.falloff > 1)
            return bad("0..1");
        return true;
    }
    std::string err;
    if (op.flag == "--map") {
        if (!session.open(op.value, err))
            kast::log::error(std::format("--map {}: {}", op.value, err));
        return err.empty();
    }
    if (op.flag == "--new-map") {
        const auto parts = split(op.value, ',');
        int w = 2, d = 2, id = mapedit::next_map_id(session.project());
        if ((parts.size() != 1 && parts.size() != 3 && parts.size() != 4) ||
            (parts.size() >= 3 && (!parseInt(parts[1], w) || !parseInt(parts[2], d))) ||
            (parts.size() == 4 && (!parseInt(parts[3], id) || id < 0 || id > 0xFFFF)))
            return bad("folder[,W,H[,id]]");
        if (!session.newMap(parts[0], parts[0], uint16_t(id), w, d, err))
            kast::log::error(std::format("--new-map {}: {}", op.value, err));
        return err.empty();
    }
    mapedit::MapDoc* doc = session.doc();
    if (!doc) {
        kast::log::error(op.flag + ": no map open (--map or --new-map first)");
        return false;
    }
    if (op.flag == "--save-map")
        return session.save();
    if (op.flag == "--sculpt") {
        const auto v = parseFloats(op.value, 4, 4);
        if (!v || (*v)[2] <= 0)
            return bad("x,z,radius,metres");
        brush.mode = (*v)[3] >= 0 ? mapedit::BrushMode::Raise : mapedit::BrushMode::Lower;
        brush.radius = (*v)[2];
        if (!sculpt(*doc, brush, (*v)[0], (*v)[1], std::fabs((*v)[3])))
            kast::log::warn(op.flag + " " + op.value + ": no change");
        return true;
    }
    if (op.flag == "--flatten") {
        const auto v = parseFloats(op.value, 4, 5);
        if (!v || (*v)[2] <= 0)
            return bad("x,z,radius,height[,fraction]");
        brush.mode = mapedit::BrushMode::Flatten;
        brush.radius = (*v)[2];
        brush.target = (*v)[3];
        if (!sculpt(*doc, brush, (*v)[0], (*v)[1], v->size() == 5 ? (*v)[4] : 1.0f))
            kast::log::warn(op.flag + " " + op.value + ": no change");
        return true;
    }
    if (const auto edit = objectEdit(*doc, op); edit)
        return *edit;
    if (const auto edit = zoneEdit(*doc, op); edit)
        return *edit;
    if (op.flag == "--add-tile" || op.flag == "--remove-tile") {
        mapedit::TileKey t;
        const auto parts = split(op.value, ',');
        const auto info = doc->info(err);
        if (parts.size() != 2 || !parseInt(parts[0], t.x) || !parseInt(parts[1], t.z) || !info)
            return bad("tx,tz");
        if (op.flag == "--remove-tile") // what the confirm dialog lists
            for (const std::string& f : mapedit::tile_files(session.project(), doc->folder, t))
                kast::log::info("Map: removing tile " + mapedit::tile_name(t) + " deletes " + f + " on save");
        bool changed = false;
        doc->edit([&](mapedit::MapState& s) {
            changed = op.flag == "--add-tile" ? mapedit::add_tile(*info, s, t) : mapedit::remove_tile(*info, s, t);
        });
        if (!changed)
            return bad(op.flag == "--add-tile"
                           ? std::format("a tile not on the map, within {} tiles a side", mapedit::kMaxMapTiles)
                           : "a tile on the map (not the last)");
        return true;
    }
    if (op.flag == "--set-spawn") {
        const auto v = parseFloats(op.value, 2, 3);
        const auto info = doc->info(err);
        if (!v || !info)
            return bad("x,z[,heading]");
        const float y = mapedit::ground_y(*info, doc->state, (*v)[0], (*v)[1]).value_or(0.0f);
        doc->edit([&](mapedit::MapState& s) {
            s.manifest["spawn"]["pos"] = mapedit::vec3_json({(*v)[0], y, (*v)[1]});
            if (v->size() == 3)
                s.manifest["spawn"]["heading"] = kast::jsonNumber((*v)[2]);
        });
        return true;
    }
    if (op.flag == "--instance") { // maxPlayers (0: no limit),emptyTimeoutSec,exitMap,x,y,z[,heading]
        const auto v = parseFloats(op.value, 6, 7);
        const auto whole = [](float f, float max) { return f >= 0 && f <= max && f == std::floor(f); };
        if (!v || !whole((*v)[0], 1e9f) || !whole((*v)[1], 31536000) || !whole((*v)[2], 0xFFFF))
            return bad("maxPlayers,emptyTimeoutSec,exitMap,x,y,z[,heading] (maxPlayers 0: no limit)");
        doc->edit([&](mapedit::MapState& s) {
            mapedit::set_instance(s, true);
            mapedit::set_max_players(s, (*v)[0] > 0 ? std::optional(uint32_t((*v)[0])) : std::nullopt);
            mapedit::set_empty_timeout(s, uint32_t((*v)[1]));
            mapedit::set_exit(s, {uint16_t((*v)[2]), {(*v)[3], (*v)[4], (*v)[5]}, v->size() == 7 ? (*v)[6] : 0.0f});
        });
        return true;
    }
    if (op.flag == "--world-map") {
        doc->edit([](mapedit::MapState& s) { mapedit::set_instance(s, false); });
        return true;
    }
    if (op.flag == "--map-id") {
        int id = 0;
        if (!parseInt(op.value, id) || id < 0 || id > 0xFFFF)
            return bad("0..65535");
        doc->edit([&](mapedit::MapState& s) { s.manifest["id"] = unsigned(id); });
        session.renumber(id); // the Terrain entity follows (main.cpp), as the Map tab's Id field does
        return true;
    }
    if (op.flag == "--material") { // project-relative .mat, or "" for the default material
        doc->edit([&](mapedit::MapState& s) {
            if (op.value.empty())
                s.manifest.erase("material");
            else
                s.manifest["material"] = op.value;
        });
        return true;
    }
    if (op.flag == "--height-range") {
        const auto v = parseFloats(op.value, 2, 2);
        if (!v)
            return bad("min,max");
        return setHeightRange(*doc, (*v)[0], (*v)[1]);
    }
    if (op.flag == "--import-png" || op.flag == "--export-png") {
        const auto v = parseTileFile(op.value);
        if (!v)
            return bad("tx,tz,file");
        return op.flag == "--import-png" ? importPng(*doc, v->first, v->second) : exportPng(*doc, v->first, v->second);
    }
    if (op.flag == "--generate-heights") {
        const auto parts = split(op.value, ',');
        mapedit::NoiseParams p;
        const auto [end, ec] = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), p.seed);
        const auto v = parts.size() > 1 ? parseFloats(op.value.substr(parts[0].size() + 1), 1, 4) : std::vector<float>();
        if (ec != std::errc() || end != parts[0].data() + parts[0].size() || !v)
            return bad("seed[,scale,octaves,persistence,lacunarity]");
        if (v->size() > 0)
            p.scale = (*v)[0];
        if (v->size() > 1)
            p.octaves = int((*v)[1]);
        if (v->size() > 2)
            p.persistence = (*v)[2];
        if (v->size() > 3)
            p.lacunarity = (*v)[3];
        if (!(p.scale > 0) || p.octaves < 1 || p.octaves > 16 || !(p.persistence > 0 && p.persistence <= 1) || !(p.lacunarity >= 1))
            return bad("scale > 0, octaves 1..16, persistence 0..1, lacunarity >= 1");
        return generateHeights(*doc, p);
    }
    if (op.flag == "--terrain-colors") { // pct 0..100 of the height range, rgb 0..1, per stop
        const auto v = parseFloats(op.value, 8, 4 * 64);
        if (!v || v->size() % 4)
            return bad("pct,r,g,b,pct,r,g,b... (at least 2 stops)");
        mapedit::Gradient g;
        for (size_t i = 0; i < v->size(); i += 4)
            g.push_back({(*v)[i] / 100.0f, {(*v)[i + 1], (*v)[i + 2], (*v)[i + 3]}});
        doc->edit([&](mapedit::MapState& s) { s.manifest["terrainColors"] = mapedit::gradient_json(g); });
        return true;
    }
    if (op.flag == "--create-terrain-material") {
        createTerrainMaterial(*doc);
        return true;
    }
    if (op.flag == "--validate") { // fails the run on any problem, writes nothing
        if (session.check())
            return true;
        kast::log::error(std::format("--validate: maps/{} has {} problem(s)", doc->folder, session.problems().size()));
        return false;
    }
    if (op.flag == "--expect-save-fail") {
        if (session.save()) {
            kast::log::error("--expect-save-fail: the save succeeded");
            return false;
        }
        return true;
    }
    if (op.flag == "--undo" || op.flag == "--redo") {
        const bool undo = op.flag == "--undo";
        if (undo ? !doc->canUndo() : !doc->canRedo()) {
            kast::log::error(std::format("{}: nothing to {}", op.flag, undo ? "undo" : "redo"));
            return false;
        }
        if (undo)
            doc->undo();
        else
            doc->redo();
        // An undone --map-id: the Terrain entity follows the manifest's id again.
        if (const auto info = doc->info(err))
            session.renumber(info->id);
        return true;
    }
    return false;
}

} // namespace

std::optional<bool> takesValue(std::string_view arg)
{
    if (std::ranges::find(kValueOps, arg) != std::end(kValueOps))
        return true;
    if (std::ranges::find(kFlagOps, arg) != std::end(kFlagOps))
        return false;
    return std::nullopt;
}

bool run(MapSession& session, const std::vector<Op>& ops)
{
    mapedit::Brush brush;
    for (const Op& op : ops)
        if (!runOp(session, op, brush))
            return false;
    return true;
}

int bakeHeadless(MapSession& session, const std::filesystem::path& settingsFile, const std::string& mapbake)
{
    MapPanel panel(settingsFile);
    if (!mapbake.empty())
        panel.setMapbake(mapbake);
    if (!panel.startBake(session))
        return 1;
    while (panel.baking()) {
        panel.pollBake();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    panel.pollBake();
    return panel.bakeExit() == 0 ? 0 : 1;
}

std::optional<std::array<float, 3>> parseFocus(std::string_view text)
{
    const auto v = parseFloats(text, 3, 3);
    return v ? std::optional(std::array<float, 3>{(*v)[0], (*v)[1], (*v)[2]}) : std::nullopt;
}

std::optional<std::array<int, 2>> parseTile(std::string_view text)
{
    const auto parts = split(text, ',');
    std::array<int, 2> t{};
    if (parts.size() != 2 || !parseInt(parts[0], t[0]) || !parseInt(parts[1], t[1]))
        return std::nullopt;
    return t;
}

float groundY(const mapedit::MapDoc* doc, float x, float z)
{
    std::string err;
    const auto info = doc ? doc->info(err) : std::nullopt;
    return info ? mapedit::ground_y(*info, doc->state, x, z).value_or(0.0f) : 0.0f;
}

void frameMap(EditorCamera& camera, const mapedit::MapDoc& doc)
{
    namespace km = kast::map;
    std::string err;
    const auto info = doc.info(err);
    if (!info)
        return;
    const auto range = km::height_range(*info);
    kast::Aabb box{{std::numeric_limits<float>::max(), range[0], std::numeric_limits<float>::max()},
                   {std::numeric_limits<float>::lowest(), range[1], std::numeric_limits<float>::lowest()}};
    for (const km::TileRect& r : info->tiles) {
        box.min = bx::min(box.min, {r.x0 * km::kTileSize, range[0], r.z0 * km::kTileSize});
        box.max = bx::max(box.max, {(r.x1 + 1) * km::kTileSize, range[1], (r.z1 + 1) * km::kTileSize});
    }
    // A map bigger than the Scene view meshes (mapedit::kEditRadius around the pivot): the meshed tiles around its centre.
    const km::TileKey c = km::tile_of((box.min.x + box.max.x) * 0.5f, (box.min.z + box.max.z) * 0.5f);
    box.min = bx::max(box.min, {(c.x - mapedit::kEditRadius) * km::kTileSize, range[0],
                                (c.z - mapedit::kEditRadius) * km::kTileSize});
    box.max = bx::min(box.max, {(c.x + mapedit::kEditRadius + 1) * km::kTileSize, range[1],
                                (c.z + mapedit::kEditRadius + 1) * km::kTileSize});
    camera.focus(box);
}

void sculptLoop(mapedit::MapDoc& doc, int frame)
{
    std::string err;
    const auto info = doc.info(err);
    if (!info)
        return;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const kast::map::TileRect& r : info->tiles) {
        lo[0] = std::min(lo[0], r.x0 * kast::map::kTileSize);
        lo[1] = std::min(lo[1], r.z0 * kast::map::kTileSize);
        hi[0] = std::max(hi[0], (r.x1 + 1) * kast::map::kTileSize);
        hi[1] = std::max(hi[1], (r.z1 + 1) * kast::map::kTileSize);
    }
    // Within the tiles meshed around a framed map's centre (mapedit::kEditRadius), so it sculpts what the view shows.
    const float meshed = float(2 * mapedit::kEditRadius + 1) * kast::map::kTileSize;
    const float angle = float(frame) * 0.01f, reach = 0.35f * std::min({hi[0] - lo[0], hi[1] - lo[1], meshed});
    const float x = (lo[0] + hi[0]) * 0.5f + reach * std::cos(angle), z = (lo[1] + hi[1]) * 0.5f + reach * std::sin(angle);
    mapedit::Brush brush;
    brush.radius = kLoopRadius;
    brush.mode = (frame / 120) % 2 ? mapedit::BrushMode::Lower : mapedit::BrushMode::Raise;
    mapedit::apply_brush(*info, doc.state, x, z, brush, 0.25f);
}

} // namespace map_cli
