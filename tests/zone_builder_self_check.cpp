// Assert-style checks for the zone builder's document, file and terrain logic (tools/zone-builder), no window. Works
// on copies of the sample maps in a temp folder. Exit 0 if every check passes. Run by CTest as zone_builder_self_check.
#include <process_runner.h>
#include <terrain_edit.h>
#include <zone_doc.h>
#include <zone_edit.h>
#include <zone_io.h>

#include <event_bus.h>
#include <files.h>
#include <scene.h>
#include <terrain.h>

#include <chrono>
#include <cmath>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // GetModuleFileNameW

#include <cstdio>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
namespace km = kast::map;
using namespace zone;

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

const fs::path kSource = KAST_SOURCE_DIR;

std::string bytes(const fs::path& file) {
    std::string out;
    kast::readFile(file, out);
    return out;
}

Json json_file(const fs::path& file) { return Json::parse(bytes(file), nullptr, false); }

// A fresh copy of a project's maps/ in the temp folder; returns the copy's root.
fs::path copy_project(const fs::path& project, const std::string& name) {
    const fs::path root = fs::temp_directory_path() / "kast_zone_self_check" / name;
    fs::remove_all(root);
    fs::create_directories(root);
    fs::copy(project / "maps", root / "maps", fs::copy_options::recursive);
    if (fs::exists(project / "materials")) fs::copy(project / "materials", root / "materials", fs::copy_options::recursive);
    return root;
}

void print_errors(const std::vector<ZoneError>& errors) {
    for (const ZoneError& e : errors) std::fprintf(stderr, "  %s: %s\n", e.file.c_str(), e.message.c_str());
}

// Load + save with no edits writes nothing; a forced rewrite of every file reads back the same JSON (so every key the
// tool does not edit survives) and the same heights, .r16 byte-identical.
void check_round_trip(const fs::path& project, const std::string& name, const std::string& folder) {
    const fs::path root = copy_project(project, name);
    const fs::path dir = root / "maps" / folder;
    Project p = scan_project(root);
    std::string err;
    auto doc = load_map(p, folder, err);
    CHECK(doc.has_value());
    if (!doc) return std::fprintf(stderr, "  %s\n", err.c_str()), void();
    CHECK(!doc->dirty());
    CHECK(validate(p, *doc).empty());

    const auto before = fs::last_write_time(dir / "map.json");
    const auto errors = save_map(p, *doc, false);
    print_errors(errors);
    CHECK(errors.empty());
    CHECK(fs::last_write_time(dir / "map.json") == before);

    const Json manifest = json_file(dir / "map.json");
    const Json areas = fs::exists(dir / "areas.json") ? json_file(dir / "areas.json") : Json();
    std::map<std::string, std::string> heightBytes;
    for (const auto& entry : fs::directory_iterator(dir / "heights")) heightBytes[entry.path().filename().string()] = bytes(entry.path());

    doc->saved = {}; // everything differs from "disk": every file is rewritten
    CHECK(save_map(p, *doc, false).empty());
    CHECK(json_file(dir / "map.json") == manifest);
    if (!areas.is_null()) CHECK(json_file(dir / "areas.json") == areas);
    for (const auto& [tile, json] : doc->state.spawns)
        CHECK(json_file(dir / ".server" / "spawns" / (tile_name(tile) + ".json")) == json);
    auto again = load_map(p, folder, err);
    CHECK(again && again->state.manifest == doc->state.manifest && again->state.areas == doc->state.areas &&
          again->state.spawns == doc->state.spawns);
    for (const auto& [file, data] : heightBytes) {
        const fs::path r16 = dir / "heights" / (fs::path(file).stem().string() + ".r16");
        if (file.ends_with(".r16")) CHECK(bytes(r16) == data);
        else CHECK(!fs::exists(dir / "heights" / file) && fs::exists(r16)); // .png replaced by .r16
    }
    if (again)
        for (const auto& [tile, heights] : doc->state.heights) CHECK(*again->state.heights.at(tile) == *heights);
}

void check_new_map() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "new_map");
    Project p = scan_project(root);
    CHECK(p.maps.size() == 2);
    const uint16_t id = next_map_id(p);
    CHECK(id != 10 && id != 11);
    std::string err;
    CHECK(!new_map(p, "meadow", "Meadow", id, 2, 2, err)); // folder taken
    CHECK(!new_map(p, "../x", "X", id, 2, 2, err));
    auto doc = new_map(p, "plains", "Plains", id, 2, 2, err);
    CHECK(doc && doc->dirty());
    if (!doc) return;
    CHECK(save_map(p, *doc, false).empty());
    CHECK(!doc->dirty());
    CHECK(p.maps.size() == 3);
    auto loaded = load_map(p, "plains", err);
    CHECK(loaded.has_value());
    std::string e;
    const auto info = loaded ? loaded->info(e) : std::nullopt;
    CHECK(info && info->id == id && info->has_tile({1, 1}) && !info->has_tile({2, 0}));

    // The engine streams the new map: a Terrain entity with radius 1 around the origin gets all 4 tiles.
    kast::TerrainStreamer streamer;
    streamer.start(root);
    CHECK(streamer.map(id) != nullptr);
    kast::Scene scene;
    const entt::entity terrain = scene.create("Terrain");
    scene.registry().emplace<kast::Terrain>(terrain, int(id), 1);
    kast::EventBus bus;
    streamer.update(scene, bus);
    int tiles = 0;
    for (auto [entity, tile] : scene.registry().view<kast::TerrainTile>().each())
        tiles += tile.data && tile.data->heights.size() == km::flat_heights().size();
    CHECK(tiles == 4);

    // Duplicate map id blocks the save and writes nothing.
    const auto mtime = fs::last_write_time(root / "maps" / "plains" / "map.json");
    loaded->edit([](ZoneState& s) { s.manifest["id"] = 10u; });
    const auto errors = save_map(p, *loaded, false);
    CHECK(errors.size() == 1 && errors[0].file == "maps/plains/map.json" && errors[0].message.find("already used by maps/meadow") != std::string::npos);
    CHECK(fs::last_write_time(root / "maps" / "plains" / "map.json") == mtime);
}

void check_undo() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "undo");
    Project p = scan_project(root);
    std::string err;
    auto doc = load_map(p, "meadow", err);
    CHECK(doc.has_value());
    if (!doc) return;
    const Json original = doc->state.manifest;
    doc->edit([](ZoneState& s) { s.manifest["name"] = "Changed"; });
    doc->edit([](ZoneState&) {}); // no change: no step
    CHECK(doc->dirty() && doc->canUndo());
    doc->undo();
    CHECK(doc->state.manifest == original && !doc->dirty() && !doc->canUndo() && doc->canRedo());
    doc->redo();
    CHECK(doc->state.manifest["name"] == "Changed");
}

void check_raycast() {
    km::MapInfo info;
    info.tiles = {{0, 0, 1, 0}};
    info.heightRange = std::array<float, 2>{0.0f, 100.0f};
    ZoneState state;
    auto h = std::make_shared<km::Heights>(km::flat_heights());
    for (auto& s : *h) s = 32768; // ~50 m
    state.heights[{0, 0}] = h;
    const float y = km::sample_height(32768, info);
    // Straight down onto tile 0_0, slanted onto flat tile 1_0 (y 0), and off the map.
    auto hit = raycast(info, state, {10.5f, 500.0f, 20.5f}, {0, -1, 0}, 1000);
    CHECK(hit && std::fabs((*hit)[1] - y) < 1e-3f && std::fabs((*hit)[0] - 10.5f) < 1e-4f);
    const float s = 1 / std::sqrt(2.0f);
    hit = raycast(info, state, {300.0f, 100.0f, 10.0f}, {s, -s, 0}, 1000);
    CHECK(hit && std::fabs((*hit)[1]) < 1e-3f && std::fabs((*hit)[0] - 400.0f) < 1e-2f);
    CHECK(!raycast(info, state, {-10.0f, 100.0f, 10.0f}, {-s, -s, 0}, 1000));
    CHECK(ground_y(info, state, 100, 100) && std::fabs(*ground_y(info, state, 100, 100) - y) < 1e-3f);
}

// Same files' content (heights compared by value, not buffer).
bool same_content(const ZoneState& a, const ZoneState& b) {
    if (a.manifest != b.manifest || a.areas != b.areas || a.spawns != b.spawns || a.heights.size() != b.heights.size())
        return false;
    for (const auto& [t, h] : a.heights)
        if (!b.heights.contains(t) || *b.heights.at(t) != *h) return false;
    return true;
}

// A flat 2 x 2 tile map, heightRange [0, 100], in memory.
ZoneDoc flat_map(Project& p) {
    std::string err;
    auto doc = new_map(p, "sculpt", "Sculpt", next_map_id(p), 2, 2, err);
    CHECK(doc.has_value());
    return std::move(*doc);
}

km::MapInfo info_of(const ZoneDoc& doc) {
    std::string err;
    return *doc.info(err);
}

float height_at(const ZoneDoc& doc, TileKey t, int u, int v) {
    return km::sample_height(tile_heights(doc.state, t)[size_t(v) * km::kTileSamples + size_t(u)], info_of(doc));
}

// Every pair of tiles sharing an edge or a corner holds the same samples there.
bool seams_equal(const ZoneDoc& doc) {
    const km::MapInfo info = info_of(doc);
    constexpr int n = km::kTileSamples;
    const auto at = [&](TileKey t, int u, int v) { return tile_heights(doc.state, t)[size_t(v) * n + size_t(u)]; };
    for (const km::TileRect& r : info.tiles)
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                for (int i = 0; i < n; ++i) {
                    if (info.has_tile({x + 1, z}) && at({x, z}, n - 1, i) != at({x + 1, z}, 0, i)) return false;
                    if (info.has_tile({x, z + 1}) && at({x, z}, i, n - 1) != at({x, z + 1}, i, 0)) return false;
                    if (info.has_tile({x + 1, z + 1}) && at({x, z}, n - 1, n - 1) != at({x + 1, z + 1}, 0, 0)) return false;
                }
    return true;
}

void check_brushes() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "brushes");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    const km::MapInfo info = info_of(doc);
    const float step = 100.0f / 65535.0f; // one quantization step of [0, 100]

    Brush brush;
    brush.radius = 10;
    brush.falloff = 0.5f; // full weight within 5 m
    doc.edit([&](ZoneState& s) { CHECK(apply_brush(info, s, 100, 100, brush, 5).size() == 1); });
    CHECK(std::fabs(height_at(doc, {0, 0}, 100, 100) - 5) <= step);
    CHECK(std::fabs(height_at(doc, {0, 0}, 103, 100) - 5) <= step); // inside the full-weight core
    const float mid = height_at(doc, {0, 0}, 107, 100);              // in the falloff
    CHECK(mid > step && mid < 5 - step);
    CHECK(height_at(doc, {0, 0}, 110, 100) == 0 && height_at(doc, {0, 0}, 120, 100) == 0); // at and past the radius
    CHECK(doc.state.heights.size() == 1); // only the touched tile has data

    brush.mode = BrushMode::Lower;
    doc.edit([&](ZoneState& s) { apply_brush(info, s, 100, 100, brush, 2); });
    CHECK(std::fabs(height_at(doc, {0, 0}, 100, 100) - 3) <= 2 * step);

    brush.mode = BrushMode::Flatten;
    brush.radius = 20;
    brush.target = 10;
    doc.edit([&](ZoneState& s) { apply_brush(info, s, 100, 100, brush, 1); });
    CHECK(std::fabs(height_at(doc, {0, 0}, 100, 100) - 10) <= step);
    CHECK(std::fabs(height_at(doc, {0, 0}, 108, 95) - 10) <= step);

    // Range: raising far past the top clamps to heightRange's max; lowering past the bottom to its min.
    brush.mode = BrushMode::Raise;
    doc.edit([&](ZoneState& s) { apply_brush(info, s, 50, 50, brush, 1000); });
    CHECK(height_at(doc, {0, 0}, 50, 50) == 100);
    brush.mode = BrushMode::Lower;
    doc.edit([&](ZoneState& s) { apply_brush(info, s, 50, 50, brush, 5000); });
    CHECK(height_at(doc, {0, 0}, 50, 50) == 0);

    // A stroke on the corner of all four tiles changes all four, and every shared edge stays equal.
    brush.mode = BrushMode::Raise;
    brush.radius = 30;
    TileSet touched;
    doc.edit([&](ZoneState& s) { touched = apply_brush(info, s, 256, 256, brush, 7); });
    CHECK(touched.size() == 4);
    CHECK(height_at(doc, {0, 0}, 256, 256) > 6.9f && height_at(doc, {1, 1}, 0, 0) == height_at(doc, {0, 0}, 256, 256));
    for (float x = 240; x < 275; x += 3.7f) doc.edit([&](ZoneState& s) { apply_brush(info, s, x, 256 - (x - 256), brush, 1.3f); });
    CHECK(seams_equal(doc));

    // The ray hits the raised ground.
    const auto hit = raycast(info, doc.state, {256, 500, 256}, {0, -1, 0}, 1000);
    CHECK(hit && std::fabs((*hit)[1] - height_at(doc, {0, 0}, 256, 256)) < 1e-3f);

    // Saving writes .r16 files for the sculpted tiles only, and they load back identically.
    CHECK(save_map(p, doc, false).empty());
    std::string err;
    const auto loaded = load_map(p, "sculpt", err);
    CHECK(loaded && loaded->state.heights.size() == 4);
    if (loaded)
        for (const auto& [t, h] : doc.state.heights) CHECK(*loaded->state.heights.at(t) == *h);
}

void check_height_range() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "range");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    km::MapInfo info = info_of(doc);
    Brush brush;
    brush.radius = 40;
    brush.falloff = 1;
    doc.edit([&](ZoneState& s) { apply_brush(info, s, 256, 100, brush, 37.5f); });
    const ZoneState before = doc.state;

    // Wider range: every world height kept within one step of the new range, nothing clipped, flat tiles move too.
    size_t clipped = 99;
    doc.edit([&](ZoneState& s) { clipped = set_height_range(info, s, -50, 200); });
    CHECK(clipped == 0);
    const km::MapInfo wide = info_of(doc);
    CHECK(km::height_range(wide)[0] == -50 && km::height_range(wide)[1] == 200);
    const float newStep = 250.0f / 65535.0f;
    bool kept = true;
    for (int tz = 0; tz < 2; ++tz)
        for (int tx = 0; tx < 2; ++tx)
            for (size_t i = 0; i < size_t(km::kTileSamples) * km::kTileSamples; ++i)
                kept &= std::fabs(km::sample_height(tile_heights(doc.state, {tx, tz})[i], wide) -
                                  km::sample_height(tile_heights(before, {tx, tz})[i], info)) <= newStep;
    CHECK(kept);
    CHECK(doc.state.heights.size() == 4); // the untouched flat tiles were at 0 m: now mid-range, so they get data
    CHECK(seams_equal(doc));

    // Narrower range: the samples outside clip to it and are counted.
    const size_t expect = count_clipped(wide, doc.state, 10, 20);
    CHECK(expect > 0);
    doc.edit([&](ZoneState& s) { clipped = set_height_range(wide, s, 10, 20); });
    CHECK(clipped == expect);
    const km::MapInfo narrow = info_of(doc);
    CHECK(height_at(doc, {0, 0}, 0, 0) == 10 && height_at(doc, {0, 0}, 256, 100) == 20);

    // Undo restores both changes exactly.
    doc.undo();
    doc.undo();
    CHECK(same_content(doc.state, before));
}

void check_png() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "png");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    const km::MapInfo info = info_of(doc);
    km::Heights ramp(size_t(km::kTileSamples) * km::kTileSamples);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = uint16_t((i * 7919) % 65536); // every bit pattern
    const std::string png = encode_png16(ramp);
    std::string err;
    const auto decoded = km::decode_heights(png, ".png", err);
    CHECK(decoded && *decoded == ramp);

    const ZoneState before = doc.state;
    bool ok = false;
    doc.edit([&](ZoneState& s) { ok = import_png16(info, s, {0, 0}, png, err); });
    CHECK(ok);
    CHECK(*doc.state.heights.at({0, 0}) == ramp);
    CHECK(seams_equal(doc)); // the neighbours' edges took the imported edge
    CHECK(encode_png16(tile_heights(doc.state, {0, 0})) == png); // export of the import: the same file

    // 8-bit, wrong size and not-a-PNG are rejected with the decoder's reason, and change nothing.
    const auto write8 = [](int w, int h) {
        std::vector<unsigned char> pixels(size_t(w) * h, 128);
        int len = 0;
        unsigned char* data = stbi_write_png_to_mem(pixels.data(), w, w, h, 1, &len);
        std::string out(reinterpret_cast<char*>(data), size_t(len));
        STBIW_FREE(data);
        return out;
    };
    const ZoneState imported = doc.state;
    CHECK(!import_png16(info, doc.state, {1, 0}, write8(257, 257), err) && err.find("8-bit") != std::string::npos);
    CHECK(!import_png16(info, doc.state, {1, 0}, write8(10, 10), err) && err.find("need 257 x 257") != std::string::npos);
    CHECK(!import_png16(info, doc.state, {1, 0}, "not a png", err) && err.starts_with("png:"));
    CHECK(same_content(doc.state, imported));

    doc.undo();
    CHECK(same_content(doc.state, before));
}

// Every file the state would write, as text: the JSON dumps and the heights' .r16 bytes.
std::string serialize(const ZoneState& s) {
    std::string out = s.manifest.dump() + "\n" + s.areas.dump() + "\n";
    for (const auto& [t, j] : s.spawns) out += tile_name(t) + ":" + j.dump() + "\n";
    for (const auto& [t, h] : s.heights) out += tile_name(t) + ":" + encode_r16(*h);
    return out;
}

std::map<std::string, fs::file_time_type> mtimes(const fs::path& dir) {
    std::map<std::string, fs::file_time_type> out;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) out[e.path().string()] = e.last_write_time();
    return out;
}

bool has_error(const std::vector<ZoneError>& errors, const std::string& file, const std::string& text) {
    for (const ZoneError& e : errors)
        if (e.file == file && e.message.find(text) != std::string::npos) return true;
    print_errors(errors);
    return false;
}

void check_areas() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "areas");
    Project p = scan_project(root);
    std::string err;
    auto loaded = load_map(p, "meadow", err);
    CHECK(loaded.has_value());
    if (!loaded) return;
    ZoneDoc& doc = *loaded;

    doc.edit([](ZoneState& s) {
        add_rect_area(s, "town", {60, 20}, {20, 60}, 5); // corners in any order
        areas_list(s)[*find_area(s, "town")]["parent"] = "meadow";
        add_polygon_area(s, "hill", {{100, 100}, {140, 100}, {120, 140}}, 6);
        areas_list(s)[*find_area(s, "hill")]["parent"] = "town";
        areas_list(s)[*find_area(s, "hill")]["portal"] = {{"map", 11u}, {"pos", vec3_json({120, 0, 130})}, {"heading", 1.5}};
    });
    CHECK(doc.state.areas["areas"][2]["rect"] == Json::parse("[20, 20, 60, 60]"));
    CHECK(validate(p, doc).empty());
    const auto areas = km::parse_areas(nlohmann::json(doc.state.areas), err);
    CHECK(areas && areas->list.size() == 4);
    if (areas) {
        const auto at = [&](float x, float z) {
            const int16_t i = km::area_at(*areas, x, 0, z);
            return i < 0 ? std::string() : areas->list[size_t(i)].id;
        };
        CHECK(at(120, 110) == "hill" && at(30, 30) == "town" && at(200, 30) == "meadow");
        CHECK(areas->list[3].portal && areas->list[3].portal->map == 11);
    }
    CHECK(save_map(p, doc, false).empty());
    auto again = load_map(p, "meadow", err);
    CHECK(again && again->state.areas == doc.state.areas);

    // Fields the tool does not edit survive renames and reshapes; children follow a rename.
    doc.edit([](ZoneState& s) {
        Json& meadow = areas_list(s)[*find_area(s, "meadow")];
        meadow["y"] = Json::parse("[-5, 50]");
        meadow["music"] = "sounds/meadow.ogg";
        meadow["flags"] = {{"pvp", true}};
        meadow["custom"] = 1;
    });
    doc.edit([](ZoneState& s) {
        rename_area(s, *find_area(s, "meadow"), "fields");
        move_area_vertex(areas_list(s)[*find_area(s, "fields")], 2, {250, 250});
    });
    const Json& fields = doc.state.areas["areas"][*find_area(doc.state, "fields")];
    CHECK(fields["y"] == Json::parse("[-5, 50]") && fields["music"] == "sounds/meadow.ogg" && fields["flags"]["pvp"] == true &&
          fields["custom"] == 1 && fields["rect"] == Json::parse("[0, 0, 250, 250]"));
    CHECK(doc.state.areas["areas"][*find_area(doc.state, "town")]["parent"] == "fields");
    CHECK(doc.state.areas["areas"][*find_area(doc.state, "cavern_gate")]["parent"] == "fields");
    CHECK(validate(p, doc).empty());

    // A polygon vertex move, then a whole-area move.
    doc.edit([](ZoneState& s) {
        move_area_vertex(areas_list(s)[*find_area(s, "hill")], 2, {120, 150});
        move_area(areas_list(s)[*find_area(s, "town")], 5, -5);
    });
    CHECK(area_points(doc.state.areas["areas"][*find_area(doc.state, "hill")])[2] == (Point{120, 150}));
    CHECK(doc.state.areas["areas"][*find_area(doc.state, "town")]["rect"] == Json::parse("[25, 15, 65, 55]"));

    // Blocked saves write nothing: a portal to an unknown map, then equal-priority overlap.
    const auto before = mtimes(root / "maps");
    doc.edit([](ZoneState& s) { areas_list(s)[*find_area(s, "hill")]["portal"]["map"] = 999u; });
    auto errors = save_map(p, doc, false);
    CHECK(has_error(errors, "maps/meadow/areas.json", "area \"hill\": portal to unknown map 999"));
    doc.undo();
    doc.edit([](ZoneState& s) { add_rect_area(s, "dup", {30, 30}, {50, 50}, 5); });
    errors = save_map(p, doc, false);
    CHECK(has_error(errors, "maps/meadow/areas.json", "overlap with equal priority 5"));
    CHECK(mtimes(root / "maps") == before);
    doc.undo();
    CHECK(validate(p, doc).empty());

    // Deleting a parent makes its children top level.
    doc.edit([](ZoneState& s) { delete_area(s, *find_area(s, "town")); });
    CHECK(!find_area(doc.state, "town") && !doc.state.areas["areas"][*find_area(doc.state, "hill")].contains("parent"));
    CHECK(validate(p, doc).empty());
}

void check_spawns() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "spawns");
    CHECK(npc_names(kSource / "samples" / "starter-mmo") == (std::vector<std::string>{"Bat", "Guard"}));
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    std::string err;
    const auto spawns_of = [&](TileKey t) {
        return km::parse_spawns(nlohmann::json(doc.state.spawns.at(t)), t, err);
    };

    // Two spawns sharing a path in tile 0_0, and a third with a different path of the same name.
    doc.edit([](ZoneState& s) {
        const SpawnRef a = add_spawn(s, "s1", "Guard", {100, 0, 100}, 0.5f, 30);
        add_path(s, a, "patrol", {{100, 0, 100}, {120, 0, 100}}, 2, true);
        const SpawnRef b = add_spawn(s, "s2", "Guard", {110, 0, 110}, 0, -1);
        spawn_json(s, b)["path"] = "patrol";
    });
    CHECK(doc.state.spawns.size() == 1 && doc.state.spawns.contains({0, 0}));
    CHECK(spawns_of({0, 0}) && spawns_of({0, 0})->size() == 2 && (*spawns_of({0, 0}))[0].path->points.size() == 2);

    // Across the border: s1 moves to 1_0's file with a copy of its path (s2 still uses the original).
    doc.edit([](ZoneState& s) { move_spawn(s, *find_spawn(s, "s1"), {300, 0, 100}); });
    CHECK(find_spawn(doc.state, "s1")->tile == (TileKey{1, 0}));
    CHECK(doc.state.spawns.at({1, 0})["paths"]["patrol"] == doc.state.spawns.at({0, 0})["paths"]["patrol"]);
    CHECK(spawn_json(doc.state, *find_spawn(doc.state, "s1"))["path"] == "patrol");
    // s2 follows: the same path is already there, so it is shared; the old file loses it with its last user.
    doc.edit([](ZoneState& s) { move_spawn(s, *find_spawn(s, "s2"), {300, 0, 120}); });
    CHECK(!doc.state.spawns.at({0, 0})["paths"].contains("patrol") && doc.state.spawns.at({0, 0})["spawns"].empty());
    CHECK(doc.state.spawns.at({1, 0})["paths"].size() == 1);
    // A different path under the same name is renamed on the way in.
    doc.edit([](ZoneState& s) {
        const SpawnRef c = add_spawn(s, "s3", "Bat", {50, 0, 50}, 0, 10);
        add_path(s, c, "patrol", {{50, 0, 50}, {60, 0, 60}, {40, 0, 60}}, 0, false);
        move_spawn(s, *find_spawn(s, "s3"), {260, 0, 50});
    });
    CHECK(spawn_json(doc.state, *find_spawn(doc.state, "s3"))["path"] == "patrol_2");
    CHECK(doc.state.spawns.at({1, 0})["paths"]["patrol_2"]["points"].size() == 3);
    CHECK(spawns_of({1, 0}) && spawns_of({1, 0})->size() == 3);
    // Path points and a heading/respawn edit keep the parser happy.
    doc.edit([](ZoneState& s) {
        add_path_point(s, *find_spawn(s, "s3"), {270, 0, 70});
        spawn_json(s, *find_spawn(s, "s3"))["respawnSec"] = -1;
    });
    CHECK(doc.state.spawns.at({1, 0})["paths"]["patrol_2"]["points"].size() == 4);
    CHECK(validate(p, doc).empty());
    CHECK(save_map(p, doc, false).empty());
    const fs::path dir = root / "maps" / "sculpt" / ".server" / "spawns";
    CHECK(fs::exists(dir / "1_0.json") && fs::exists(dir / "0_0.json") && !fs::exists(dir / "0_1.json"));
    auto again = load_map(p, "sculpt", err);
    CHECK(again && again->state.spawns == doc.state.spawns);

    // Blocked: a duplicate id across files, and a spawn off the map's tiles. Nothing is written.
    const auto before = mtimes(root / "maps");
    doc.edit([](ZoneState& s) { add_spawn(s, "s1", "Bat", {100, 0, 300}, 0, 10); });
    CHECK(has_error(save_map(p, doc, false), "maps/sculpt/.server/spawns/0_1.json", "spawn \"s1\": duplicate id on this map"));
    doc.undo();
    doc.edit([](ZoneState& s) { add_spawn(s, "far", "Bat", {600, 0, 100}, 0, 10); });
    CHECK(has_error(save_map(p, doc, false), "maps/sculpt/.server/spawns/2_0.json", "tile is not on the map"));
    CHECK(mtimes(root / "maps") == before);
    doc.undo();

    // Deleting a path unassigns it.
    doc.edit([](ZoneState& s) { delete_path(s, {1, 0}, "patrol"); });
    CHECK(!spawn_json(doc.state, *find_spawn(doc.state, "s1")).contains("path") && validate(p, doc).empty());
}

void check_tiles() {
    // Rects: a full square stays one rect; an L becomes a run per shape change.
    CHECK(tile_rects({{0, 0}, {1, 0}, {0, 1}, {1, 1}}) == Json::parse("[[0, 0, 1, 1]]"));
    CHECK(tile_rects({{0, 0}, {1, 0}, {2, 0}, {0, 1}}) == Json::parse("[[0, 0, 2, 0], [0, 1, 0, 1]]"));
    CHECK(tile_rects({{0, 0}, {0, 2}}) == Json::parse("[[0, 0, 0, 0], [0, 2, 0, 2]]"));

    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "tiles");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    km::MapInfo info = info_of(doc);
    Brush brush;
    brush.radius = 30;
    doc.edit([&](ZoneState& s) {
        apply_brush(info, s, 500, 200, brush, 20); // a hill on the right edge of tile 1_0 and 1_1 (x 512)
        apply_brush(info, s, 400, 400, brush, 10);
        add_spawn(s, "keeper", "Guard", {400, 0, 400}, 0, 5);
    });
    CHECK(save_map(p, doc, false).empty());
    const fs::path dir = root / "maps" / "sculpt";
    CHECK(fs::exists(dir / "heights" / "1_1.r16") && fs::exists(dir / ".server" / "spawns" / "1_1.json"));

    // Adding 2_0 takes the hill's edge, so the map has no seam.
    doc.edit([&](ZoneState& s) { CHECK(add_tile(info, s, {2, 0})); });
    CHECK(!add_tile(info_of(doc), doc.state, {2, 0})); // already declared
    info = info_of(doc);
    CHECK(info.has_tile({2, 0}) && doc.state.heights.contains({2, 0}) && seams_equal(doc));
    CHECK(height_at(doc, {2, 0}, 0, 200) == height_at(doc, {1, 0}, 256, 200) && height_at(doc, {2, 0}, 0, 200) > 10);
    CHECK(height_at(doc, {2, 0}, 100, 200) == 0); // inside: flat at heightMin

    // Removing 1_1: the files that go are listed, its spawn is dropped, and saving deletes them.
    CHECK(tile_files(p, "sculpt", {1, 1}) ==
          (std::vector<std::string>{"maps/sculpt/heights/1_1.r16", "maps/sculpt/.server/spawns/1_1.json"}));
    doc.edit([&](ZoneState& s) { CHECK(remove_tile(info, s, {1, 1})); });
    CHECK(removed_tile_files(p, doc) == tile_files(p, "sculpt", {1, 1}));
    CHECK(!find_spawn(doc.state, "keeper") && !info_of(doc).has_tile({1, 1}));
    CHECK(save_map(p, doc, false).empty());
    CHECK(!fs::exists(dir / "heights" / "1_1.r16") && !fs::exists(dir / ".server" / "spawns" / "1_1.json"));
    CHECK(fs::exists(dir / "heights" / "2_0.r16"));
    std::string err;
    auto again = load_map(p, "sculpt", err);
    CHECK(again && again->info(err)->tiles.size() == 2 && !again->info(err)->has_tile({1, 1}));

    // Removed and added back before saving: flat again, so its old heights file must not be read back.
    info = info_of(doc);
    doc.edit([&](ZoneState& s) { remove_tile(info, s, {2, 0}); });
    doc.edit([&](ZoneState& s) { add_tile(info_of(doc), s, {2, 0}); });
    CHECK(doc.state.heights.contains({2, 0})); // it took 1_0's hill edge again
    doc.edit([&](ZoneState& s) {
        remove_tile(info_of(doc), s, {1, 0}); // now 2_0 has no shaped neighbour
        remove_tile(info_of(doc), s, {2, 0});
        add_tile(info_of(doc), s, {2, 0});
    });
    CHECK(!doc.state.heights.contains({2, 0}));
    CHECK(save_map(p, doc, false).empty());
    CHECK(!fs::exists(dir / "heights" / "2_0.r16") && !fs::exists(dir / "heights" / "1_0.r16"));
    CHECK(!remove_tile(info_of(doc), doc.state, {5, 5})); // not declared
}

void check_process_runner() {
    ProcessRunner run;
    std::string err;
    // This program as the child (main's --child): an argument with a space and a quote must arrive whole.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    CHECK(run.start(self, {"--child", "two words \"quoted\""}, err));
    while (run.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto lines = run.takeLines();
    CHECK(run.exitCode() == 3 && lines == (std::vector<std::string>{"child: two words \"quoted\"", "child: stderr"}));
    CHECK(!run.start(kSource / "no_such.exe", {}, err) && err.starts_with("cannot start"));

    // kast_mapbake on a saved tool-made map, if services/world is built.
    const fs::path exe = find_mapbake("");
    if (exe.empty()) {
        std::printf("kast_mapbake not built: bake check skipped\n");
        return;
    }
    CHECK(find_mapbake(exe.string()) == exe && find_mapbake("C:/nope/kast_mapbake.exe").empty());
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "bake");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    CHECK(save_map(p, doc, false).empty());
    CHECK(run.start(exe, {root / "maps" / "sculpt"}, err));
    while (run.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(run.exitCode() == 0 && fs::exists(root / "maps" / "sculpt" / ".server" / "nav" / "1_1.navtile"));
}

// Each kind of edit undoes to the exact files before it and redoes to the exact files after.
void check_undo_each() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "undo_each");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    const km::MapInfo info = info_of(doc);
    const std::string png = encode_png16(km::Heights(size_t(km::kTileSamples) * km::kTileSamples, 1234));
    std::string err;
    Brush brush;
    const std::vector<std::pair<const char*, std::function<void(ZoneState&)>>> edits = {
        {"raise", [&](ZoneState& s) { apply_brush(info, s, 256, 256, brush, 5); }},
        {"flatten", [&](ZoneState& s) { brush.mode = BrushMode::Flatten; brush.target = 2; apply_brush(info, s, 256, 256, brush, 1); }},
        {"import", [&](ZoneState& s) { import_png16(info, s, {1, 1}, png, err); }},
        {"height range", [&](ZoneState& s) { set_height_range(info, s, -20, 80); }},
        {"manifest", [](ZoneState& s) { s.manifest["name"] = "Renamed"; s.manifest["material"] = "materials/stone.mat"; }},
        {"add area", [](ZoneState& s) { add_rect_area(s, "a", {10, 10}, {90, 90}, 1); }},
        {"move vertex", [](ZoneState& s) { move_area_vertex(areas_list(s)[0], 1, {120, 5}); }},
        {"portal", [](ZoneState& s) { areas_list(s)[0]["portal"] = {{"map", 10u}, {"pos", vec3_json({128, 0, 100})}, {"heading", 0}}; }},
        {"add spawn", [](ZoneState& s) { add_spawn(s, "x", "Guard", {100, 0, 100}, 0, 5); }},
        {"add path", [](ZoneState& s) { add_path(s, *find_spawn(s, "x"), "p", {{100, 0, 100}, {150, 0, 100}}, 1, false); }},
        {"move spawn", [](ZoneState& s) { move_spawn(s, *find_spawn(s, "x"), {400, 0, 400}); }},
        {"delete spawn", [](ZoneState& s) { delete_spawn(s, *find_spawn(s, "x")); }},
        {"delete area", [](ZoneState& s) { delete_area(s, 0); }},
        {"add tile", [&](ZoneState& s) { add_tile(info, s, {2, 1}); }},
        {"remove tile", [&](ZoneState& s) { remove_tile(info, s, {0, 1}); }},
        {"map spawn", [](ZoneState& s) { s.manifest["spawn"]["pos"] = vec3_json({300, 1, 300}); }},
    };
    for (const auto& [name, fn] : edits) {
        const std::string before = serialize(doc.state);
        doc.edit(fn);
        const std::string after = serialize(doc.state);
        doc.undo();
        const bool undone = serialize(doc.state) == before;
        doc.redo();
        const bool redone = serialize(doc.state) == after;
        if (before == after || !undone || !redone) std::fprintf(stderr, "  undo/redo of \"%s\"\n", name);
        CHECK(before != after && undone && redone);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--child") { // check_process_runner's child: stdout, stderr, exit code
        std::printf("child: %s\n", argv[2]);
        std::fflush(stdout);
        std::fprintf(stderr, "child: stderr\n");
        return 3;
    }
    check_round_trip(kSource / "samples" / "starter-mmo", "rt_meadow", "meadow");
    check_round_trip(kSource / "samples" / "starter-mmo", "rt_cavern", "cavern");
    check_round_trip(kSource / "tests" / "project", "rt_hills", "hills");
    check_new_map();
    check_undo();
    check_raycast();
    check_brushes();
    check_height_range();
    check_png();
    check_areas();
    check_spawns();
    check_undo_each();
    check_tiles();
    check_process_runner();
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("zone builder self-check: all passed\n");
    return failures ? 1 : 0;
}
