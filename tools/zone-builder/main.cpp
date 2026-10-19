// kast_zone_builder: opens a Kast project and creates and edits its zone maps (maps/<folder>/) in 3D, saving them in
// the formats the engine and the world server load (tools/zone-builder/docs/index.html). The command line opens a project and a
// map and runs edits in order, for tests and scripts.
#include <zone/zone_edit.h>
#include "zone_panels.h"
#include "zone_view.h"

#include <imgui_bgfx.h>
#include <ui_common.h>

#include <app.h>
#include <crash_dialogs.h>
#include <log.h>
#include <scene_io.h> // jsonNumber

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h> // DockBuilder API

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <thread>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using zone::Json;
using zone::Session;

constexpr bgfx::ViewId kSceneViews = 1; // shadow maps, scene, post (ZoneView)
constexpr bgfx::ViewId kImGuiView = 255;

constexpr const char* kUsage =
    "Usage: kast_zone_builder [--project dir] [--map folder] [edits...] [--frames N] [--screenshot out.png] "
    "[--size WxH] [--window-pos X,Y] [--no-vsync] [--log-fps] [--sculpt-loop] [--headless] [--no-recycle] "
    "[--focus x,z,distance] [--panel name] [--select id] [--mapbake exe] [--bake] [--prompt-remove-tile tx,tz]\n"
    "Edits, run in order: --new-map folder[,W,H[,id]] --falloff F --sculpt x,z,radius,metres (negative lowers) "
    "--flatten x,z,radius,height[,fraction] --height-range min,max --map-id N --material path --import-png tx,tz,file --export-png tx,tz,file "
    "--add-area id,priority,x0,z0,x1,z1 --add-polygon id,priority,x,z,x,z,x,z... --area-parent id,parent "
    "--area-portal id,map,x,y,z,heading --move-area-vertex id,i,x,z --delete-area id "
    "--add-spawn id,npc,x,z[,heading[,respawnSec]] --move-spawn id,x,z --delete-spawn id "
    "--add-path spawnId,wait,loop,x,z,x,z... --add-tile tx,tz --remove-tile tx,tz --set-spawn x,z[,heading] "
    "--add-object type,prefab,x,z[,yaw[,scale[,box,hx,hy,hz|cylinder,r,h]]] --set-interactable id,range,event[,name[,data]] "
    "--move-object tx,tz,i,x,z --delete-object tx,tz,i --scatter x,z,radius,count,seed,scaleMin,scaleMax,prefab[+prefab] "
    "--erase-visuals x,z,radius "
    "--generate-heights seed[,scale,octaves,persistence,lacunarity] --terrain-colors pct,r,g,b,pct,r,g,b... "
    "--create-terrain-material --save --validate --expect-save-fail --undo --redo";
// Flags that are edits taking a value, run in command-line order.
constexpr std::string_view kValueOps[] = {"--map", "--new-map", "--falloff", "--sculpt", "--flatten", "--height-range",
                                          "--import-png", "--export-png", "--material",
                                          "--generate-heights", "--terrain-colors",
                                          "--add-area", "--add-polygon", "--area-parent", "--area-portal",
                                          "--move-area-vertex", "--delete-area", "--add-spawn", "--move-spawn",
                                          "--delete-spawn", "--add-path", "--map-id", "--add-tile",
                                          "--remove-tile", "--set-spawn", "--add-object", "--set-interactable",
                                          "--move-object", "--delete-object", "--scatter", "--erase-visuals"};
constexpr float kLoopRadius = 24.0f; // --sculpt-loop's brush

// One command-line edit: the flag and its value (empty for flags without one).
struct Op {
    std::string flag, value;
};

struct Options {
    fs::path project;
    std::vector<Op> ops; // --map, --new-map, --save, ... in command-line order
    int frames = 0;      // 0 = run until closed
    std::string screenshot;
    std::optional<std::pair<int, int>> size, windowPos;
    bool noVsync = false;
    bool headless = false; // run the edits, no window
    bool noRecycle = false;
    bool logFps = false;
    bool sculptLoop = false; // raise and lower a moving brush every frame (the --log-fps sculpting benchmark)
    std::optional<std::array<float, 3>> focus; // camera pivot x, z and distance, for screenshots
    std::string panel, select;                 // bring this panel to the front / select this area or spawn
    std::string mapbake;                       // kast_mapbake.exe for this run (else the setting, else the default)
    std::optional<zone::TileKey> promptRemove; // open the remove-tile confirm dialog, for screenshots
    bool bake = false;                         // after the edits: Bake Navmesh (the window waits for it)
};

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t end = text.find(sep, start);
        parts.emplace_back(text.substr(start, end - start));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

bool parseInt(std::string_view text, int& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

bool parseFloat(std::string_view text, float& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size() && std::isfinite(out);
}

// "a,b,c": exactly min..max finite numbers.
std::optional<std::vector<float>> parseFloats(std::string_view text, size_t min, size_t max) {
    std::vector<float> out;
    for (const std::string& part : split(text, ',')) {
        if (!parseFloat(part, out.emplace_back())) return std::nullopt;
    }
    if (out.size() < min || out.size() > max) return std::nullopt;
    return out;
}

// "tx,tz,file": the file may itself contain commas.
std::optional<std::pair<zone::TileKey, fs::path>> parseTileFile(std::string_view text) {
    const auto parts = split(text, ',');
    zone::TileKey t;
    if (parts.size() < 3 || !parseInt(parts[0], t.x) || !parseInt(parts[1], t.z)) return std::nullopt;
    const std::string_view file = text.substr(parts[0].size() + parts[1].size() + 2);
    return std::pair{t, fs::path(std::u8string(file.begin(), file.end()))};
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--project" && hasValue) {
            const std::string_view v = argv[++i];
            options.project = fs::path(std::u8string(v.begin(), v.end()));
        } else if (std::find(std::begin(kValueOps), std::end(kValueOps), arg) != std::end(kValueOps) && hasValue) {
            options.ops.push_back({std::string(arg), argv[++i]});
        } else if (arg == "--save" || arg == "--expect-save-fail" || arg == "--undo" || arg == "--redo" || arg == "--validate" ||
                   arg == "--create-terrain-material") {
            options.ops.push_back({std::string(arg), {}});
        } else if (arg == "--frames" && hasValue && parseInt(argv[i + 1], options.frames) && options.frames > 0) {
            ++i;
        } else if (arg == "--screenshot" && hasValue) {
            options.screenshot = argv[++i];
        } else if (arg == "--size" && hasValue) {
            const auto parts = split(argv[++i], 'x');
            int w = 0, h = 0;
            if (parts.size() != 2 || !parseInt(parts[0], w) || !parseInt(parts[1], h) || w <= 0 || h <= 0) return std::nullopt;
            options.size = {w, h};
        } else if (arg == "--window-pos" && hasValue) {
            options.windowPos = kast::parseWindowPos(argv[++i]);
            if (!options.windowPos) return std::nullopt;
        } else if (arg == "--no-vsync") {
            options.noVsync = true;
        } else if (arg == "--focus" && hasValue) {
            const auto v = parseFloats(argv[++i], 3, 3);
            if (!v) return std::nullopt;
            options.focus = std::array<float, 3>{(*v)[0], (*v)[1], (*v)[2]};
        } else if (arg == "--panel" && hasValue) {
            options.panel = argv[++i];
        } else if (arg == "--select" && hasValue) {
            options.select = argv[++i];
        } else if (arg == "--mapbake" && hasValue) {
            options.mapbake = argv[++i];
        } else if (arg == "--prompt-remove-tile" && hasValue) {
            const auto parts = split(argv[++i], ',');
            zone::TileKey t;
            if (parts.size() != 2 || !parseInt(parts[0], t.x) || !parseInt(parts[1], t.z)) return std::nullopt;
            options.promptRemove = t;
        } else if (arg == "--bake") {
            options.bake = true;
        } else if (arg == "--log-fps") {
            options.logFps = true;
        } else if (arg == "--sculpt-loop") {
            options.sculptLoop = true;
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--no-recycle") {
            options.noRecycle = true;
        } else {
            kast::log::error(std::format("Unknown or incomplete argument '{}'", arg));
            return std::nullopt;
        }
    }
    if (!options.screenshot.empty() && options.frames == 0) {
        kast::log::error("--screenshot requires --frames N");
        return std::nullopt;
    }
    if (!options.ops.empty() && options.project.empty()) {
        kast::log::error("edits need --project <dir>");
        return std::nullopt;
    }
    return options;
}

// The area, spawn and path edits (one undo step each); nullopt if op is not one of them. Positions snap to the ground.
std::optional<bool> objectEdit(Session& session, const Op& op) {
    zone::ZoneDoc& doc = *session.doc;
    const auto parts = split(op.value, ',');
    const auto bad = [&](const char* form) -> std::optional<bool> {
        kast::log::error(std::format("{} {}: expected {}", op.flag, op.value, form));
        return false;
    };
    // parts[from..] as floats, or nullopt.
    const auto floats = [&](size_t from) -> std::optional<std::vector<float>> {
        std::vector<float> out;
        for (size_t i = from; i < parts.size(); ++i)
            if (!parseFloat(parts[i], out.emplace_back())) return std::nullopt;
        return out;
    };
    std::string err;
    const auto info = doc.info(err);
    const auto ground = [&](float x, float z) {
        return zone::Vec3{x, info ? zone::ground_y(*info, doc.state, x, z).value_or(0.0f) : 0.0f, z};
    };
    const auto area = [&](const std::string& id) { return zone::find_area(doc.state, id); };
    const auto spawn = [&](const std::string& id) { return zone::find_spawn(doc.state, id); };
    int priority = 0;

    if (op.flag == "--add-area") {
        const auto v = parts.size() == 6 && parseInt(parts[1], priority) ? floats(2) : std::nullopt;
        if (!v) return bad("id,priority,x0,z0,x1,z1");
        doc.edit([&](zone::ZoneState& s) { zone::add_rect_area(s, parts[0], {(*v)[0], (*v)[1]}, {(*v)[2], (*v)[3]}, priority); });
        return true;
    }
    if (op.flag == "--add-polygon") {
        const auto v = parts.size() >= 8 && parts.size() % 2 == 0 && parseInt(parts[1], priority) ? floats(2) : std::nullopt;
        if (!v) return bad("id,priority,x,z,x,z,x,z[,...]");
        std::vector<zone::Point> points;
        for (size_t i = 0; i < v->size(); i += 2) points.push_back({(*v)[i], (*v)[i + 1]});
        doc.edit([&](zone::ZoneState& s) { zone::add_polygon_area(s, parts[0], points, priority); });
        return true;
    }
    if (op.flag == "--area-parent") {
        if (parts.size() != 2 || !area(parts[0])) return bad("id,parent (an existing area)");
        doc.edit([&](zone::ZoneState& s) { zone::areas_list(s)[*area(parts[0])]["parent"] = parts[1]; });
        return true;
    }
    if (op.flag == "--area-portal") {
        int map = 0;
        const auto v = parts.size() == 6 && parseInt(parts[1], map) && map >= 0 && map <= 0xFFFF ? floats(2) : std::nullopt;
        if (!v || !area(parts[0])) return bad("id,map,x,y,z,heading (an existing area)");
        doc.edit([&](zone::ZoneState& s) {
            zone::areas_list(s)[*area(parts[0])]["portal"] = {{"map", unsigned(map)},
                                                              {"pos", zone::vec3_json({(*v)[0], (*v)[1], (*v)[2]})},
                                                              {"heading", kast::jsonNumber((*v)[3])}};
        });
        return true;
    }
    if (op.flag == "--move-area-vertex") {
        int index = 0;
        const auto v = parts.size() == 4 && parseInt(parts[1], index) && index >= 0 ? floats(2) : std::nullopt;
        if (!v || !area(parts[0])) return bad("id,vertex,x,z (an existing area)");
        doc.edit([&](zone::ZoneState& s) { zone::move_area_vertex(zone::areas_list(s)[*area(parts[0])], size_t(index), {(*v)[0], (*v)[1]}); });
        return true;
    }
    if (op.flag == "--delete-area") {
        if (!area(op.value)) return bad("id (an existing area)");
        doc.edit([&](zone::ZoneState& s) { zone::delete_area(s, *area(op.value)); });
        return true;
    }
    if (op.flag == "--add-spawn") {
        const auto v = parts.size() >= 4 && parts.size() <= 6 ? floats(2) : std::nullopt;
        if (!v) return bad("id,npc,x,z[,heading[,respawnSec]]");
        doc.edit([&](zone::ZoneState& s) {
            zone::add_spawn(s, parts[0], parts[1], ground((*v)[0], (*v)[1]), v->size() > 2 ? (*v)[2] : 0.0f,
                            v->size() > 3 ? (*v)[3] : 60.0f);
        });
        return true;
    }
    // Zone objects: addressed by tile and index in its objects file ("tx,tz,i"), interactables by id.
    const auto objectAt = [&](size_t from) -> std::optional<zone::ObjectRef> {
        int tx = 0, tz = 0, index = 0;
        if (parts.size() < from + 3 || !parseInt(parts[from], tx) || !parseInt(parts[from + 1], tz) || !parseInt(parts[from + 2], index))
            return std::nullopt;
        const zone::ObjectRef ref{{tx, tz}, size_t(index)};
        const auto refs = zone::all_objects(doc.state);
        return std::find(refs.begin(), refs.end(), ref) != refs.end() ? std::optional(ref) : std::nullopt;
    };
    if (op.flag == "--add-object") {
        const std::string form = "type,prefab,x,z[,yaw[,scale[,box,hx,hy,hz | cylinder,r,h]]]";
        const std::vector<std::string> types = {"visual", "obstacle", "interactable"};
        const auto type = parts.empty() ? types.end() : std::find(types.begin(), types.end(), parts[0]);
        const size_t shapeAt = std::min<size_t>(parts.size(), 6);
        std::vector<float> v;
        for (size_t i = 2; i < shapeAt; ++i)
            if (!parseFloat(parts[i], v.emplace_back())) return bad(form.c_str());
        const auto shape = parts.size() > 6 ? floats(7) : std::optional<std::vector<float>>(std::vector<float>{});
        const bool box = parts.size() > 6 && parts[6] == "box" && shape && shape->size() == 3;
        const bool cylinder = parts.size() > 6 && parts[6] == "cylinder" && shape && shape->size() == 2;
        if (type == types.end() || parts.size() < 4 || v.size() < 2 || (parts.size() > 6 && !box && !cylinder)) return bad(form.c_str());
        doc.edit([&](zone::ZoneState& s) {
            const zone::ObjectRef ref = zone::add_object(s, kast::map::ObjectType(type - types.begin()), parts[1],
                                                         ground(v[0], v[1]), v.size() > 2 ? v[2] : 0.0f, v.size() > 3 ? v[3] : 1.0f);
            Json& o = zone::object_json(s, ref);
            if (box) o["shape"] = {{"box", {{"halfExtents", zone::vec3_json({(*shape)[0], (*shape)[1], (*shape)[2]})}}}};
            if (cylinder) o["shape"] = {{"cylinder", {{"radius", kast::jsonNumber((*shape)[0])}, {"height", kast::jsonNumber((*shape)[1])}}}};
            if (o.value("type", "") == "interactable") zone::sync_interactable(s, ref);
        });
        return true;
    }
    if (op.flag == "--set-interactable") { // id,range,event[,name[,data]]: data is the rest, commas and all
        int id = 0;
        float range = 0;
        std::optional<zone::ObjectRef> ref;
        for (const zone::ObjectRef& r : zone::all_objects(doc.state))
            if (parts.size() >= 3 && parseInt(parts[0], id) && zone::object_json(doc.state, r).value("id", Json()) == id) ref = r;
        Json data;
        if (parts.size() > 4) {
            const size_t at = parts[0].size() + parts[1].size() + parts[2].size() + parts[3].size() + 4;
            data = Json::parse(op.value.substr(at), nullptr, false);
        }
        if (!ref || !parseFloat(parts[1], range) || (parts.size() > 4 && !data.is_object()))
            return bad("id,range,event[,name[,data JSON object]] (an existing interactable)");
        doc.edit([&](zone::ZoneState& s) {
            if (parts.size() > 3 && !parts[3].empty()) zone::object_json(s, *ref)["name"] = parts[3];
            zone::sync_interactable(s, *ref);
            Json& entry = *zone::interactable_json(s, ref->tile, uint32_t(id));
            entry["range"] = kast::jsonNumber(range);
            entry["event"] = parts[2];
            if (data.is_object()) entry["data"] = data;
        });
        return true;
    }
    if (op.flag == "--move-object") {
        const auto ref = objectAt(0);
        const auto v = ref && parts.size() == 5 ? floats(3) : std::nullopt;
        if (!v) return bad("tx,tz,index,x,z (an existing object)");
        doc.edit([&](zone::ZoneState& s) { zone::move_object(s, *ref, ground((*v)[0], (*v)[1])); });
        return true;
    }
    if (op.flag == "--delete-object") {
        const auto ref = objectAt(0);
        if (!ref || parts.size() != 3) return bad("tx,tz,index (an existing object)");
        doc.edit([&](zone::ZoneState& s) { zone::delete_object(s, *ref); });
        return true;
    }
    if (op.flag == "--scatter") { // one stroke of one dab, as the Scatter tool's
        zone::Scatter brush;
        int count = 0, seed = 0;
        std::vector<float> f; // x, z, radius, scaleMin, scaleMax
        for (const size_t i : {0, 1, 2, 5, 6})
            if (i < parts.size() && !parseFloat(parts[i], f.emplace_back())) f.pop_back();
        if (parts.size() != 8 || f.size() != 5 || !parseInt(parts[3], count) || !parseInt(parts[4], seed) || count < 1 || !info)
            return bad("x,z,radius,count,seed,scaleMin,scaleMax,prefab[+prefab...]");
        brush.radius = f[2];
        brush.count = count;
        brush.scaleMin = f[3];
        brush.scaleMax = f[4];
        brush.prefabs = split(parts[7], '+');
        zone::ScatterRng rng{uint32_t(seed)};
        size_t placed = 0;
        doc.edit([&](zone::ZoneState& s) { placed = zone::scatter(*info, s, brush, rng, f[0], f[1]); });
        session.print(std::format("--scatter: {} visual(s) placed", placed));
        return true;
    }
    if (op.flag == "--erase-visuals") {
        const auto v = parts.size() == 3 ? floats(0) : std::nullopt;
        if (!v) return bad("x,z,radius");
        size_t erased = 0;
        doc.edit([&](zone::ZoneState& s) { erased = zone::erase_visuals(s, (*v)[0], (*v)[1], (*v)[2]); });
        session.print(std::format("--erase-visuals: {} visual(s) removed", erased));
        return true;
    }
    if (op.flag == "--move-spawn") {
        const auto v = parts.size() == 3 ? floats(1) : std::nullopt;
        if (!v || !spawn(parts[0])) return bad("id,x,z (an existing spawn)");
        doc.edit([&](zone::ZoneState& s) { zone::move_spawn(s, *spawn(parts[0]), ground((*v)[0], (*v)[1])); });
        return true;
    }
    if (op.flag == "--delete-spawn") {
        if (!spawn(op.value)) return bad("id (an existing spawn)");
        doc.edit([&](zone::ZoneState& s) { zone::delete_spawn(s, *spawn(op.value)); });
        return true;
    }
    if (op.flag == "--add-path") {
        int loop = 0;
        const auto v = parts.size() >= 5 && parts.size() % 2 == 1 && parseInt(parts[2], loop) ? floats(3) : std::nullopt;
        float wait = 0;
        if (!v || !parseFloat(parts[1], wait) || !spawn(parts[0])) return bad("spawnId,wait,loop(0|1),x,z[,x,z...]");
        std::vector<zone::Vec3> points;
        for (size_t i = 0; i < v->size(); i += 2) points.push_back(ground((*v)[i], (*v)[i + 1]));
        doc.edit([&](zone::ZoneState& s) { zone::add_path(s, *spawn(parts[0]), parts[0] + "_path", points, wait, loop != 0); });
        return true;
    }
    return std::nullopt;
}

// Runs one command-line edit; false (logged) stops the run with exit code 1. brush: --falloff's, for later sculpts.
bool runOp(Session& session, const Op& op, zone::Brush& brush) {
    const auto bad = [&](const char* form) {
        kast::log::error(std::format("{} {}: expected {}", op.flag, op.value, form));
        return false;
    };
    if (op.flag == "--falloff") {
        if (!parseFloat(op.value, brush.falloff) || brush.falloff < 0 || brush.falloff > 1) return bad("0..1");
        return true;
    }
    if (op.flag == "--map") return session.openMap(op.value);
    if (op.flag == "--new-map") {
        const auto parts = split(op.value, ',');
        int w = 2, d = 2, id = session.project ? zone::next_map_id(*session.project) : 0;
        if ((parts.size() != 1 && parts.size() != 3 && parts.size() != 4) ||
            (parts.size() >= 3 && (!parseInt(parts[1], w) || !parseInt(parts[2], d))) ||
            (parts.size() == 4 && (!parseInt(parts[3], id) || id < 0 || id > 0xFFFF))) {
            kast::log::error("--new-map folder[,W,H[,id]]");
            return false;
        }
        return session.newMap(parts[0], parts[0], uint16_t(id), w, d);
    }
    if (!session.doc) {
        kast::log::error(op.flag + ": no map open (--map or --new-map first)");
        return false;
    }
    if (op.flag == "--save") return session.save();
    if (op.flag == "--sculpt") {
        const auto v = parseFloats(op.value, 4, 4);
        if (!v || (*v)[2] <= 0) return bad("x,z,radius,metres");
        brush.mode = (*v)[3] >= 0 ? zone::BrushMode::Raise : zone::BrushMode::Lower;
        brush.radius = (*v)[2];
        if (!session.sculpt(brush, (*v)[0], (*v)[1], std::fabs((*v)[3]))) kast::log::warn(op.flag + " " + op.value + ": no change");
        return true;
    }
    if (op.flag == "--flatten") {
        const auto v = parseFloats(op.value, 4, 5);
        if (!v || (*v)[2] <= 0) return bad("x,z,radius,height[,fraction]");
        brush.mode = zone::BrushMode::Flatten;
        brush.radius = (*v)[2];
        brush.target = (*v)[3];
        if (!session.sculpt(brush, (*v)[0], (*v)[1], v->size() == 5 ? (*v)[4] : 1.0f))
            kast::log::warn(op.flag + " " + op.value + ": no change");
        return true;
    }
    if (const auto edit = objectEdit(session, op); edit) return *edit;
    if (op.flag == "--add-tile" || op.flag == "--remove-tile") {
        zone::TileKey t;
        const auto parts = split(op.value, ',');
        std::string err;
        const auto info = session.doc->info(err);
        if (parts.size() != 2 || !parseInt(parts[0], t.x) || !parseInt(parts[1], t.z) || !info) return bad("tx,tz");
        if (op.flag == "--remove-tile" && session.project) // what the confirm dialog lists
            for (const std::string& f : zone::tile_files(*session.project, session.doc->folder, t))
                session.print("Removing tile " + zone::tile_name(t) + " deletes " + f + " on save");
        bool changed = false;
        session.doc->edit([&](zone::ZoneState& s) {
            changed = op.flag == "--add-tile" ? zone::add_tile(*info, s, t) : zone::remove_tile(*info, s, t);
        });
        if (!changed) return bad(op.flag == "--add-tile" ? "a tile not on the map" : "a tile on the map (not the last)");
        return true;
    }
    if (op.flag == "--set-spawn") {
        const auto v = parseFloats(op.value, 2, 3);
        std::string err;
        const auto info = session.doc->info(err);
        if (!v || !info) return bad("x,z[,heading]");
        const float y = zone::ground_y(*info, session.doc->state, (*v)[0], (*v)[1]).value_or(0.0f);
        session.doc->edit([&](zone::ZoneState& s) {
            s.manifest["spawn"]["pos"] = zone::vec3_json({(*v)[0], y, (*v)[1]});
            if (v->size() == 3) s.manifest["spawn"]["heading"] = kast::jsonNumber((*v)[2]);
        });
        return true;
    }
    if (op.flag == "--map-id") {
        int id = 0;
        if (!parseInt(op.value, id) || id < 0 || id > 0xFFFF) return bad("0..65535");
        session.doc->edit([&](zone::ZoneState& s) { s.manifest["id"] = unsigned(id); });
        return true;
    }
    if (op.flag == "--material") { // project-relative .mat, or "" for the default material
        session.doc->edit([&](zone::ZoneState& s) {
            if (op.value.empty()) s.manifest.erase("material");
            else s.manifest["material"] = op.value;
        });
        return true;
    }
    if (op.flag == "--height-range") {
        const auto v = parseFloats(op.value, 2, 2);
        if (!v) return bad("min,max");
        return session.setHeightRange((*v)[0], (*v)[1]);
    }
    if (op.flag == "--import-png" || op.flag == "--export-png") {
        const auto v = parseTileFile(op.value);
        if (!v) return bad("tx,tz,file");
        return op.flag == "--import-png" ? session.importPng(v->first, v->second) : session.exportPng(v->first, v->second);
    }
    if (op.flag == "--generate-heights") {
        const auto parts = split(op.value, ',');
        zone::NoiseParams p;
        const auto [end, ec] = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), p.seed);
        const auto v = parts.size() > 1 ? parseFloats(op.value.substr(parts[0].size() + 1), 1, 4) : std::vector<float>();
        if (ec != std::errc() || end != parts[0].data() + parts[0].size() || !v) return bad("seed[,scale,octaves,persistence,lacunarity]");
        if (v->size() > 0) p.scale = (*v)[0];
        if (v->size() > 1) p.octaves = int((*v)[1]);
        if (v->size() > 2) p.persistence = (*v)[2];
        if (v->size() > 3) p.lacunarity = (*v)[3];
        if (!(p.scale > 0) || p.octaves < 1 || p.octaves > 16 || !(p.persistence > 0 && p.persistence <= 1) || !(p.lacunarity >= 1))
            return bad("scale > 0, octaves 1..16, persistence 0..1, lacunarity >= 1");
        return session.generateHeights(p);
    }
    if (op.flag == "--terrain-colors") { // pct 0..100 of the height range, rgb 0..1, per stop
        const auto v = parseFloats(op.value, 8, 4 * 64);
        if (!v || v->size() % 4) return bad("pct,r,g,b,pct,r,g,b... (at least 2 stops)");
        zone::Gradient g;
        for (size_t i = 0; i < v->size(); i += 4) g.push_back({(*v)[i] / 100.0f, {(*v)[i + 1], (*v)[i + 2], (*v)[i + 3]}});
        return session.setTerrainColors(g);
    }
    if (op.flag == "--create-terrain-material") return session.createTerrainMaterial();
    if (op.flag == "--validate") { // fails the run on any problem, writes nothing
        session.problems = zone::validate(*session.project, *session.doc);
        for (const zone::ZoneError& e : session.problems) kast::log::error(e.file + ": " + e.message);
        return session.problems.empty();
    }
    if (op.flag == "--expect-save-fail") {
        if (session.save()) {
            kast::log::error("--expect-save-fail: the save succeeded");
            return false;
        }
        return true;
    }
    if (op.flag == "--undo") {
        if (!session.doc->canUndo()) return kast::log::error("--undo: nothing to undo"), false;
        session.doc->undo();
        return true;
    }
    if (op.flag == "--redo") {
        if (!session.doc->canRedo()) return kast::log::error("--redo: nothing to redo"), false;
        session.doc->redo();
        return true;
    }
    return false;
}

// One frame of --sculpt-loop: a brush circling the map's centre (crossing tile borders), raising for 2 s then lowering
// for 2 s. Straight on the state, outside undo: it is a benchmark, not an edit.
void sculptLoop(zone::ZoneDoc& doc, const zone::ZoneView& view, int frame) {
    std::string err;
    const auto info = doc.info(err);
    if (!info) return;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const kast::map::TileRect& r : info->tiles) {
        lo[0] = std::min(lo[0], r.x0 * kast::map::kTileSize);
        lo[1] = std::min(lo[1], r.z0 * kast::map::kTileSize);
        hi[0] = std::max(hi[0], (r.x1 + 1) * kast::map::kTileSize);
        hi[1] = std::max(hi[1], (r.z1 + 1) * kast::map::kTileSize);
    }
    const float angle = float(frame) * 0.01f, reach = 0.35f * std::min(hi[0] - lo[0], hi[1] - lo[1]);
    const float x = (lo[0] + hi[0]) * 0.5f + reach * std::cos(angle), z = (lo[1] + hi[1]) * 0.5f + reach * std::sin(angle);
    zone::Brush brush;
    brush.radius = kLoopRadius;
    brush.mode = (frame / 120) % 2 ? zone::BrushMode::Lower : zone::BrushMode::Raise;
    zone::apply_brush(*info, doc.state, x, z, brush, 0.25f);
    view.drawGroundCircle(doc, x, z, brush.radius, IM_COL32(255, 204, 0, 255));
}

void buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, int(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
    ImGuiID leftBottom = 0;
    const ImGuiID leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.35f, nullptr, &leftBottom);
    ImGui::DockBuilderDockWindow("Maps", leftTop);
    ImGui::DockBuilderDockWindow("Map", leftBottom);
    ImGui::DockBuilderDockWindow("Terrain", leftBottom);
    ImGui::DockBuilderDockWindow("Areas", leftBottom);
    ImGui::DockBuilderDockWindow("Spawns", leftBottom);
    ImGui::DockBuilderDockWindow("Objects", leftBottom);
    ImGui::DockBuilderDockWindow("###Problems", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Bake", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

} // namespace

int main(int argc, char** argv) {
    kast::disableCrashDialogsIfRequested();
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        kast::log::error(kUsage);
        return 2;
    }

    Session session;
    session.recycle = !options->noRecycle;
    if (char* pref = SDL_GetPrefPath("Kast", "ZoneBuilder")) {
        session.settingsFile = fs::path(std::u8string(reinterpret_cast<const char8_t*>(pref))) / "zone_builder.json";
        SDL_free(pref);
    }
    session.loadSettings();
    if (!options->mapbake.empty()) session.mapbakeSetting = options->mapbake; // this run only: not saved
    if (!options->project.empty() && !session.openProject(options->project)) return 1;
    zone::Brush brush;
    for (const Op& op : options->ops)
        if (!runOp(session, op, brush)) return 1;
    if (options->headless) {
        if (!options->bake) return 0;
        if (!session.startBake()) return 1;
        while (session.bake.running()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        session.pollBake();
        for (const std::string& line : session.bakeLog) kast::log::info(line);
        return session.bakeExit == 0 ? 0 : 1;
    }

    zone::ZoneView view(kSceneViews);
    zone::Panels panels(view);
    UiScale uiScale(std::nullopt);
    FrameLimit frames(options->frames, options->screenshot);
    SDL_Window* mainWindow = nullptr;
    std::string iniFile;

    kast::AppHooks hooks;
    hooks.init = [&](SDL_Window* window) {
        mainWindow = window;
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // Test runs (--frames) use the default layout, not whatever the last session left.
        if (options->frames == 0) {
            if (char* pref = SDL_GetPrefPath("Kast", "ZoneBuilder")) {
                iniFile = std::string(pref) + "imgui.ini";
                SDL_free(pref);
            }
        }
        io.IniFilename = iniFile.empty() ? nullptr : iniFile.c_str();
        applyTheme();
        loadFont();
        uiScale.init(window);
        if (!ImGui_ImplSDL3_InitForOther(window)) {
            kast::log::error("ImGui SDL3 backend init failed");
            ImGui::DestroyContext();
            return false;
        }
        imgui_bgfx::init(kImGuiView);
        bgfx::setViewClear(kImGuiView, BGFX_CLEAR_NONE); // over the 3D view, which fills the backbuffer
        view.init();
        if (session.doc) view.frameMap(*session.doc);
        if (options->focus && session.doc) {
            std::string err;
            const auto info = session.doc->info(err);
            const float x = (*options->focus)[0], z = (*options->focus)[1];
            view.camera.pivot = {x, info ? zone::ground_y(*info, session.doc->state, x, z).value_or(0.0f) : 0.0f, z};
            view.camera.distance = (*options->focus)[2];
            view.camera.pitch = -50.0f;
        }
        if (!options->panel.empty()) panels.showPanel(options->panel);
        if (!options->select.empty()) panels.select(session, options->select);
        if (options->promptRemove) panels.askRemoveTile(*options->promptRemove);
        if (options->bake && !session.startBake()) return false;
        return true;
    };
    hooks.event = [&](const SDL_Event& event) {
        uiScale.onEvent(event);
        ImGui_ImplSDL3_ProcessEvent(&event);
    };
    FpsLog fpsLog;
    int loopFrame = 0;
    int bakeFrames = 0; // -1 once reported
    hooks.frame = [&] {
        const uint64_t frameStart = SDL_GetPerformanceCounter();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiID dockspace = ImGui::GetID("ZoneDockSpace");
        if (!ImGui::DockBuilderGetNode(dockspace) || !ImGui::DockBuilderGetNode(dockspace)->IsSplitNode())
            buildDefaultLayout(dockspace);
        ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(mainWindow, &width, &height);
        view.updateCamera(ImGui::GetIO().DisplaySize.y);
        panels.draw(session, mainWindow);
        const fs::path root = session.project ? session.project->root : fs::path();
        view.render(session.doc ? &*session.doc : nullptr, root, uint16_t(width), uint16_t(height));
        if (session.doc) view.drawOverlays(*session.doc);
        if (options->sculptLoop && session.doc) sculptLoop(*session.doc, view, loopFrame++);

        ImGui::Render();
        bgfx::touch(kImGuiView);
        darkenTextOnAccent(ImGui::GetDrawData());
        imgui_bgfx::render(ImGui::GetDrawData());
        // --bake: the frame count starts once the bake is done, so the screenshot shows its result; the frames drawn
        // meanwhile show the UI kept going.
        if (options->bake && (session.bake.running() || !session.bakeExit)) {
            ++bakeFrames;
        } else {
            if (options->bake && bakeFrames >= 0) {
                kast::log::info(std::format("{} frames drawn while kast_mapbake ran", bakeFrames));
                bakeFrames = -1;
            }
            frames.endFrame();
        }
        if (options->logFps) fpsLog.endFrame(frameStart);
    };
    hooks.screenshotSaved = [&](const char*, bool saved) { frames.onScreenshotSaved(saved); };
    hooks.quitRequested = [&] { return panels.onQuitRequested(session) || options->frames != 0; };
    hooks.shutdown = [&] {
        view.shutdown();
        imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    };

    const int code = kast::run({.title = "Kast Zone Builder",
                                .width = options->size ? options->size->first : 1600,
                                .height = options->size ? options->size->second : 900,
                                .vsync = !options->noVsync,
                                .activate = options->frames == 0,
                                .position = options->windowPos},
                               hooks);
    if (code != 0) return code;
    if (options->bake && session.bakeExit != 0) return 1;
    return frames.failed() ? 1 : 0;
}
