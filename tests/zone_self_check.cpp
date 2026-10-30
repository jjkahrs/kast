// Assert-style checks for the zone builder's document, file and terrain logic (engine/zone), no window. Works
// on copies of the sample maps in a temp folder. Exit 0 if every check passes. Run by CTest as zone_self_check.
#include <zone/process_runner.h>
#include <zone/terrain_edit.h>
#include <zone/zone_doc.h>
#include <zone/zone_edit.h>
#include <zone/zone_io.h>

#include <crash_dialogs.h>

#include <asset_ref.h>
#include <event_bus.h>
#include <files.h>
#include <scene.h>
#include <renderer.h> // decodeImage
#include <terrain.h>
#include <tile_scenes.h>

#include <algorithm>
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
#include <fstream>
#include <functional>
#include <tuple>
#include <map>
#include <set>
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

    Json manifest = json_file(dir / "map.json");
    kast::readAssetRefs(manifest); // its {id, path} refs: saved below without the editor's ids, as plain paths
    const Json areas = fs::exists(dir / "areas.json") ? json_file(dir / "areas.json") : Json();
    std::map<std::string, std::string> heightBytes;
    std::error_code noHeights; // a flat map (the crypt) has no heights/ folder
    for (const auto& entry : fs::directory_iterator(dir / "heights", noHeights))
        if (entry.path().extension() != ".meta") heightBytes[entry.path().filename().string()] = bytes(entry.path()); // not the editor's asset ids

    doc->saved = {}; // everything differs from "disk": every file is rewritten
    if (doc->state.manifest.contains("material")) doc->saved.manifest = Json{{"material", doc->state.manifest["material"]}}; // still owns its generated terrain files
    CHECK(save_map(p, *doc, false).empty());
    CHECK(json_file(dir / "map.json") == manifest);
    if (!areas.is_null()) CHECK(json_file(dir / "areas.json") == areas);
    for (const auto& [tile, json] : doc->state.spawns)
        CHECK(json_file(dir / ".server" / "spawns" / (tile_name(tile) + ".json")) == json);
    for (const auto& [tile, json] : doc->state.objects) CHECK(json_file(dir / "objects" / (tile_name(tile) + ".json")) == json);
    for (const auto& [tile, json] : doc->state.interactables)
        CHECK(json_file(dir / ".server" / "interactables" / (tile_name(tile) + ".json")) == json);
    auto again = load_map(p, folder, err);
    CHECK(again && again->state.manifest == doc->state.manifest && again->state.areas == doc->state.areas &&
          again->state.spawns == doc->state.spawns && again->state.objects == doc->state.objects &&
          again->state.interactables == doc->state.interactables);
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
    // The size cap: kMaxMapTiles a side is made, one more (or none) refused with the range in the message.
    const std::string range = "1 to " + std::to_string(kMaxMapTiles) + " each way";
    CHECK(new_map(p, "big", "Big", id, kMaxMapTiles, kMaxMapTiles, err));
    for (const auto [w, d] : {std::pair{kMaxMapTiles + 1, 1}, {1, kMaxMapTiles + 1}, {0, 2}, {2, 0}}) {
        err.clear();
        CHECK(!new_map(p, "big", "Big", id, w, d, err) && err.find(range) != std::string::npos);
    }
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
    for (const auto& [t, j] : s.objects) out += "objects " + tile_name(t) + ":" + j.dump() + "\n";
    for (const auto& [t, j] : s.interactables) out += "interactables " + tile_name(t) + ":" + j.dump() + "\n";
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

// The instance fields (docs/editor/zone-builder.html#instance): a world map made an instance map, its fields set, saved
// and read back; undo returns it to the world map it was; validation as the server's, the exit against the project.
void check_instance() {
    const fs::path root = copy_project(kSource / "tests" / "project", "instance");
    Project p = scan_project(root);
    std::string err;
    auto loaded = load_map(p, "cave", err);
    CHECK(loaded.has_value());
    if (!loaded) return;
    ZoneDoc& doc = *loaded;
    const Json before = doc.state.manifest;
    const std::string file = "maps/cave/map.json";

    doc.edit([](ZoneState& s) { set_instance(s, true, {1, {1030, 0, 1060}, 3.14f}); });
    doc.edit([](ZoneState& s) { set_max_players(s, 4); });
    doc.edit([](ZoneState& s) { set_empty_timeout(s, 60); });
    CHECK(doc.state.manifest["type"] == "instance" && doc.state.manifest["maxPlayers"] == 4 &&
          doc.state.manifest["emptyTimeoutSec"] == 60 && doc.state.manifest["exit"]["map"] == 1);
    print_errors(validate(p, doc));
    CHECK(validate(p, doc).empty());
    CHECK(doc.history().undoName() == "Edit Map");
    CHECK(save_map(p, doc, false).empty());
    auto again = load_map(p, "cave", err);
    const auto info = again ? again->info(err) : std::nullopt;
    CHECK(info && info->instanced() && info->maxPlayers == 4u && info->emptyTimeoutSec == 60 && info->exit &&
          info->exit->map == 1 && info->exit->pos == (std::array<float, 3>{1030, 0, 1060}) && info->exit->heading == 3.14f);
    CHECK(again && again->state.manifest == doc.state.manifest); // round trip: what the panel wrote is what loads
    // A second save with no edits writes nothing; set_instance on an instance map keeps its exit.
    CHECK(again && !again->dirty());
    doc.edit([](ZoneState& s) { set_instance(s, true, {9, {0, 0, 0}, 0}); });
    CHECK(doc.state.manifest["exit"]["map"] == 1);

    // Undo back to the world map, field by field; redo again.
    for (int i = 0; i < 3; ++i) doc.undo();
    CHECK(doc.state.manifest == before && !doc.state.manifest.contains("type"));
    for (int i = 0; i < 3; ++i) doc.redo();
    CHECK(doc.state.manifest["maxPlayers"] == 4);
    doc.edit([](ZoneState& s) { set_max_players(s, std::nullopt); });
    CHECK(!doc.state.manifest.contains("maxPlayers") && validate(p, doc).empty()); // unlimited

    // The exit as the server checks it: unknown map, off its tiles, inside a portal there, on an instance map.
    const std::pair<km::Portal, const char*> exits[] = {
        {{99, {10, 0, 10}, 0}, "\"exit\": to unknown map 99"},
        {{1, {5000, 0, 10}, 0}, "\"exit\": position is not on a tile of map 1"},
        {{1, {1045, 0, 995}, 0}, "\"exit\": position is inside portal area \"cave_mouth\" of map 1"},
        {{6, {128, 0, 128}, 0}, "\"exit\": map 6 is an instance map; an exit must be on a world map"},
        {{2, {10, 0, 10}, 0}, "\"exit\": map 2 is an instance map"}, // itself
    };
    for (const auto& [exit, want] : exits) {
        doc.edit([&](ZoneState& s) { set_exit(s, exit); });
        CHECK(has_error(validate(p, doc), file, want));
    }
    doc.edit([](ZoneState& s) { set_exit(s, {1, {1030, 0, 1060}, 0}); });
    CHECK(validate(p, doc).empty());

    // Bad values and misplaced fields: the manifest's parse error, file and field named (the panel's map.json: line).
    doc.edit([](ZoneState& s) { s.manifest["maxPlayers"] = 0; });
    CHECK(has_error(validate(p, doc), file, "\"maxPlayers\" must be an integer >= 1"));
    CHECK(save_map(p, doc, false).size() == 1); // blocked: nothing written
    doc.undo();
    doc.edit([](ZoneState& s) { s.manifest.erase("exit"); });
    CHECK(has_error(validate(p, doc), file, "\"exit\" must be {"));
    doc.undo();
    doc.edit([](ZoneState& s) { set_instance(s, false); });
    CHECK(!doc.state.manifest.contains("exit") && !doc.state.manifest.contains("emptyTimeoutSec") && validate(p, doc).empty());
    doc.edit([](ZoneState& s) { s.manifest["emptyTimeoutSec"] = 5; });
    CHECK(has_error(validate(p, doc), file, "\"emptyTimeoutSec\" is only for instance maps"));

    // The sample crypt (an instance map) and the start map's portal into it validate.
    auto crypt = load_map(p, "crypt", err);
    CHECK(crypt && crypt->info(err)->instanced() && validate(p, *crypt).empty());
    auto start = load_map(p, "start", err);
    CHECK(start && validate(p, *start).empty());
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
    CHECK(npc_names(kSource / "samples" / "starter-mmo") == (std::vector<std::string>{"Bat", "Elder", "Guard"}));
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

// Zone objects: every type placed, edited, moved across a tile border and deleted, each interactable's server entry in
// step; unknown keys survive a save and load; validate() blocks a save on a bad shape, a missing server entry or a
// duplicate id; an emptied tile's files are deleted.
// Asset refs (engine/asset_ref.h): with the editor's idOf hook, map.json's material and the objects' prefabs are saved as
// {"id", "path"} (areas.json and the server's files never); the zone loaders, the engine and the server read them back
// as paths.
void check_asset_refs() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "refs");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    doc.edit([&](ZoneState& s) {
        add_object(s, km::ObjectType::Visual, "prefabs/grass.prefab", {10, 0, 10}, 0.5f, 0.8f);
        s.manifest["material"] = "materials/ground.mat";
    });
    kast::setAssetRefHooks({.idOf = [](const std::string& path) -> std::optional<std::string> {
        return path.ends_with(".prefab") || path.ends_with(".mat") ? std::optional("id:" + path) : std::nullopt;
    }});
    const auto errors = save_map(p, doc, false);
    kast::setAssetRefHooks({});
    print_errors(errors);
    CHECK(errors.empty());
    const fs::path dir = root / "maps" / "sculpt";
    const Json manifest = json_file(dir / "map.json"), objects = json_file(dir / "objects" / "0_0.json");
    CHECK(manifest["material"] == (Json{{"id", "id:materials/ground.mat"}, {"path", "materials/ground.mat"}}));
    CHECK(objects["objects"][0]["prefab"] == (Json{{"id", "id:prefabs/grass.prefab"}, {"path", "prefabs/grass.prefab"}}));
    std::string err;
    const auto info = km::parse_manifest(nlohmann::json(manifest), err); // as the server reads the file
    CHECK(info && info->material == "materials/ground.mat");
    const auto parsed = km::parse_objects(nlohmann::json(objects), {0, 0});
    CHECK(parsed.errors.empty() && parsed.objects.size() == 1 && parsed.objects[0].prefab == "prefabs/grass.prefab");
    auto again = load_map(scan_project(root), "sculpt", err);
    CHECK(again && again->state.manifest["material"] == "materials/ground.mat" &&
          again->state.objects.at({0, 0})["objects"][0]["prefab"] == "prefabs/grass.prefab");
}

void check_objects() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "objects");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    using km::ObjectType;
    ObjectRef rock, shrine;
    doc.edit([&](ZoneState& s) {
        add_object(s, ObjectType::Visual, "prefabs/grass.prefab", {10, 0, 10}, 0.5f, 0.8f);
        rock = add_object(s, ObjectType::Obstacle, "prefabs/rock.prefab", {20, 0, 20}, 0, 1);
        shrine = add_object(s, ObjectType::Interactable, "prefabs/shrine.prefab", {30, 0, 30}, 1, 1);
    });
    CHECK(all_objects(doc.state).size() == 3 && doc.state.objects.size() == 1);
    CHECK(object_json(doc.state, rock)["shape"] == default_shape() && !object_json(doc.state, rock).contains("id"));
    const Json* entry = interactable_json(doc.state, {0, 0}, 1);
    CHECK(object_json(doc.state, shrine)["id"] == 1 && entry && (*entry)["event"] == "shrine.activated" && (*entry)["range"] == 3 &&
          (*entry)["pos"] == object_json(doc.state, shrine)["pos"] && (*entry)["shape"] == default_shape());
    CHECK(next_interactable_id(doc.state) == 2 && validate(p, doc).empty());
    const auto parsed = km::parse_objects(nlohmann::json(doc.state.objects.at({0, 0})), {0, 0});
    CHECK(parsed.errors.empty() && parsed.objects.size() == 3);

    // Server-only fields and unknown keys stay through edits, a move across the border and a save + load.
    doc.edit([&](ZoneState& s) {
        Json& e = *interactable_json(s, {0, 0}, 1);
        e["event"] = "altar.used";
        e["data"] = {{"gold", 5}};
        e["note"] = "kept";
        object_json(s, shrine)["name"] = "Altar";
        object_json(s, shrine)["tint"] = "blue";
        sync_interactable(s, shrine);
        shrine = move_object(s, shrine, {300, 0, 40});
    });
    CHECK(shrine.tile == (TileKey{1, 0}) && !interactable_json(doc.state, {0, 0}, 1) && !doc.state.interactables.contains({0, 0}));
    entry = interactable_json(doc.state, {1, 0}, 1);
    CHECK(entry && (*entry)["event"] == "altar.used" && (*entry)["data"]["gold"] == 5 && (*entry)["note"] == "kept" &&
          (*entry)["name"] == "Altar" && (*entry)["pos"] == vec3_json({300, 0, 40}));
    CHECK(object_json(doc.state, shrine)["tint"] == "blue" && validate(p, doc).empty());
    print_errors(save_map(p, doc, false));
    const fs::path dir = root / "maps" / "sculpt";
    CHECK(fs::exists(dir / "objects" / "0_0.json") && fs::exists(dir / "objects" / "1_0.json") &&
          fs::exists(dir / ".server" / "interactables" / "1_0.json") && !fs::exists(dir / ".server" / "interactables" / "0_0.json"));
    std::string err;
    auto again = load_map(p, "sculpt", err);
    CHECK(again && again->state.objects == doc.state.objects && again->state.interactables == doc.state.interactables);

    // Type changes: an obstacle made interactable gets id 2 and a server entry; back to obstacle loses both.
    doc.edit([&](ZoneState& s) { set_object_type(s, rock, ObjectType::Interactable); });
    CHECK(object_json(doc.state, rock)["id"] == 2 && interactable_json(doc.state, {0, 0}, 2));
    doc.edit([&](ZoneState& s) { set_interactable_id(s, rock, 7); });
    CHECK(object_json(doc.state, rock)["id"] == 7 && interactable_json(doc.state, {0, 0}, 7) && !interactable_json(doc.state, {0, 0}, 2));
    doc.edit([&](ZoneState& s) { set_object_type(s, rock, ObjectType::Obstacle); });
    CHECK(!object_json(doc.state, rock).contains("id") && !doc.state.interactables.contains({0, 0}) &&
          object_json(doc.state, rock).contains("shape"));

    // Blocked saves, nothing written.
    const auto before = mtimes(root / "maps");
    doc.edit([&](ZoneState& s) { object_json(s, rock)["shape"] = {{"cylinder", {{"radius", -1}, {"height", 1}}}}; });
    CHECK(has_error(save_map(p, doc, false), "maps/sculpt/objects/0_0.json", "objects[1]: \"shape.cylinder\" must be"));
    doc.undo();
    doc.edit([&](ZoneState& s) { s.interactables.erase({1, 0}); });
    CHECK(has_error(save_map(p, doc, false), "maps/sculpt/objects/1_0.json", "interactable 1 has no entry in .server/interactables/1_0.json"));
    doc.undo();
    doc.edit([&](ZoneState& s) {
        const ObjectRef twin = add_object(s, ObjectType::Interactable, "prefabs/shrine.prefab", {280, 0, 60}, 0, 1);
        set_interactable_id(s, twin, 1);
    });
    CHECK(has_error(save_map(p, doc, false), "maps/sculpt/objects/1_0.json", "duplicate id"));
    CHECK(mtimes(root / "maps") == before);
    doc.undo();

    // Deleting the last object of a tile deletes its files on save.
    doc.edit([&](ZoneState& s) { delete_object(s, shrine); });
    CHECK(!doc.state.objects.contains({1, 0}) && !doc.state.interactables.contains({1, 0}));
    print_errors(save_map(p, doc, false));
    CHECK(!fs::exists(dir / "objects" / "1_0.json") && !fs::exists(dir / ".server" / "interactables" / "1_0.json") &&
          fs::exists(dir / "objects" / "0_0.json"));
}

// The scatter brush: the same seed and dabs give the same objects, another seed others; only visuals, on the ground,
// within the radius and the scale range; a stroke is one undo step; erase removes visuals only.
void check_scatter() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "scatter");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    const km::MapInfo info = info_of(doc);
    doc.edit([&](ZoneState& s) { apply_brush(info, s, 200, 200, Brush{.radius = 60}, 8); }); // a hill: y varies
    Scatter brush;
    brush.prefabs = {"prefabs/grass.prefab", "prefabs/flower.prefab"};
    brush.radius = 10;
    brush.count = 40;
    brush.scaleMin = 0.5f;
    brush.scaleMax = 1.5f;
    const auto stroke = [&](uint32_t seed) {
        ZoneState s = doc.state;
        ScatterRng rng(seed);
        for (const Point at : {Point{200, 200}, Point{215, 200}, Point{250, 210}}) scatter(info, s, brush, rng, at[0], at[1]);
        return s;
    };
    const ZoneState a = stroke(7), b = stroke(7), c = stroke(8);
    CHECK(a.objects == b.objects && !a.objects.empty() && a.objects != c.objects);
    CHECK(all_objects(a).size() == 120); // every point lands on the 2 x 2 map
    std::set<std::string> prefabs;
    for (const ObjectRef& ref : all_objects(a)) {
        const Json& o = object_json(a, ref);
        const float x = o["pos"][0], y = o["pos"][1], z = o["pos"][2], scale = o["scale"];
        prefabs.insert(o["prefab"]);
        CHECK(object_type(o) == km::ObjectType::Visual && scale >= 0.5f && scale <= 1.5f);
        CHECK(std::fabs(y - *ground_y(info, a, x, z)) < 1e-3f);
        CHECK(std::hypot(x - 200, z - 200) <= 10.01f || std::hypot(x - 215, z - 200) <= 10.01f || std::hypot(x - 250, z - 210) <= 10.01f);
    }
    CHECK(prefabs.size() == 2);
    // In the document: one undo step per stroke, as the tool does it (begin on mouse down, commit on mouse up).
    doc.begin();
    ScatterRng rng(7);
    for (const Point at : {Point{200, 200}, Point{215, 200}, Point{250, 210}}) scatter(info, doc.state, brush, rng, at[0], at[1]);
    doc.commit();
    CHECK(doc.state.objects == a.objects);
    doc.undo();
    CHECK(doc.state.objects.empty());
    doc.redo();
    // Erase: visuals within 12 m of (200, 200) go, an obstacle there stays.
    doc.edit([](ZoneState& s) { add_object(s, km::ObjectType::Obstacle, "prefabs/rock.prefab", {201, 0, 201}, 0, 1); });
    const size_t total = all_objects(doc.state).size();
    size_t erased = 0;
    doc.edit([&](ZoneState& s) { erased = erase_visuals(s, 200, 200, 12); });
    CHECK(erased > 0 && all_objects(doc.state).size() == total - erased);
    for (const ObjectRef& ref : all_objects(doc.state)) {
        const Json& o = object_json(doc.state, ref);
        const float x = o["pos"][0], z = o["pos"][2];
        CHECK(object_type(o) != km::ObjectType::Visual || std::hypot(x - 200, z - 200) > 12);
    }
    CHECK(validate(p, doc).empty());
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

// Each kind of edit undoes to the exact files before it and redoes to the exact files after, as one step named for
// what it changed.
void check_undo_each() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "undo_each");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    const km::MapInfo info = info_of(doc);
    const std::string png = encode_png16(km::Heights(size_t(km::kTileSamples) * km::kTileSamples, 1234));
    std::string err;
    Brush brush;
    const auto first_object = [](const ZoneState& s) { return all_objects(s).front(); };
    const std::vector<std::tuple<const char*, const char*, std::function<void(ZoneState&)>>> edits = {
        {"raise", "Edit Terrain", [&](ZoneState& s) { apply_brush(info, s, 256, 256, brush, 5); }},
        {"flatten", "Edit Terrain", [&](ZoneState& s) { brush.mode = BrushMode::Flatten; brush.target = 2; apply_brush(info, s, 256, 256, brush, 1); }},
        {"import", "Edit Terrain", [&](ZoneState& s) { import_png16(info, s, {1, 1}, png, err); }},
        {"height range", "Edit Terrain", [&](ZoneState& s) { set_height_range(info, s, -20, 80); }},
        {"manifest", "Edit Map", [](ZoneState& s) { s.manifest["name"] = "Renamed"; s.manifest["material"] = "materials/stone.mat"; }},
        {"add area", "Edit Areas", [](ZoneState& s) { add_rect_area(s, "a", {10, 10}, {90, 90}, 1); }},
        {"move vertex", "Edit Areas", [](ZoneState& s) { move_area_vertex(areas_list(s)[0], 1, {120, 5}); }},
        {"portal", "Edit Areas", [](ZoneState& s) { areas_list(s)[0]["portal"] = {{"map", 10u}, {"pos", vec3_json({128, 0, 100})}, {"heading", 0}}; }},
        {"add spawn", "Edit Spawns", [](ZoneState& s) { add_spawn(s, "x", "Guard", {100, 0, 100}, 0, 5); }},
        {"add path", "Edit Spawns", [](ZoneState& s) { add_path(s, *find_spawn(s, "x"), "p", {{100, 0, 100}, {150, 0, 100}}, 1, false); }},
        {"move spawn", "Edit Spawns", [](ZoneState& s) { move_spawn(s, *find_spawn(s, "x"), {400, 0, 400}); }},
        {"delete spawn", "Edit Spawns", [](ZoneState& s) { delete_spawn(s, *find_spawn(s, "x")); }},
        {"delete area", "Edit Areas", [](ZoneState& s) { delete_area(s, 0); }},
        {"add object", "Edit Objects", [](ZoneState& s) { add_object(s, km::ObjectType::Interactable, "prefabs/chest.prefab", {100, 0, 100}, 0, 1); }},
        {"move object", "Edit Objects", [&](ZoneState& s) { move_object(s, first_object(s), {400, 0, 400}); }}, // into another tile
        {"delete object", "Edit Objects", [&](ZoneState& s) { delete_object(s, first_object(s)); }},
        {"add tile", "Edit Terrain", [&](ZoneState& s) { add_tile(info, s, {2, 1}); }},
        {"remove tile", "Edit Terrain", [&](ZoneState& s) { remove_tile(info, s, {0, 1}); }},
        {"map spawn", "Edit Map", [](ZoneState& s) { s.manifest["spawn"]["pos"] = vec3_json({300, 1, 300}); }},
    };
    for (const auto& [name, step, fn] : edits) {
        const std::string before = serialize(doc.state);
        const size_t steps = doc.history().size();
        doc.edit(fn);
        const std::string after = serialize(doc.state);
        const bool named = doc.history().size() == steps + 1 && doc.history().undoName() == step;
        doc.undo();
        const bool undone = serialize(doc.state) == before;
        doc.redo();
        const bool redone = serialize(doc.state) == after;
        if (before == after || !undone || !redone || !named) std::fprintf(stderr, "  undo/redo of \"%s\"\n", name);
        CHECK(before != after && undone && redone && named);
    }
}

// The zone's history (docs/editor/undo-redo.html): a stroke is one step, no-op edits record nothing, undo is refused
// mid-stroke, a new edit discards the redo branch, 100 steps at most, the saved state is clean again after undo / redo,
// and each map (each loaded doc) has its own steps.
void check_undo_history() {
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "undo_history");
    Project p = scan_project(root);
    ZoneDoc doc = flat_map(p);
    ZoneDoc capped = flat_map(p); // before doc is saved over the folder new_map wants
    CHECK(!doc.canUndo() && !doc.canRedo() && doc.history().size() == 0);
    const km::MapInfo info = info_of(doc);
    const auto dab = [&](ZoneState& s, float x) { apply_brush(info, s, x, 256, Brush{}, 2); };

    // A stroke of several dabs: one step; undo is refused until it ends.
    const std::string start = serialize(doc.state);
    doc.begin();
    for (float x = 100; x <= 400; x += 20) dab(doc.state, x);
    const std::string stroked = serialize(doc.state);
    doc.undo();
    CHECK(doc.editing() && serialize(doc.state) == stroked && !doc.canUndo());
    doc.commit();
    CHECK(doc.history().size() == 1 && doc.history().undoName() == "Edit Terrain");
    doc.undo();
    CHECK(serialize(doc.state) == start && doc.canRedo());
    doc.redo();
    CHECK(serialize(doc.state) == stroked);

    // No-op edits: nothing changed, or changed and changed back within one gesture (a field typed and restored).
    doc.edit([](ZoneState&) {});
    doc.edit([](ZoneState& s) {
        const Json name = s.manifest["name"];
        s.manifest["name"] = "Typing";
        s.manifest["name"] = name;
    });
    CHECK(doc.history().size() == 1);

    // A new edit after undo discards the redo branch.
    doc.edit([](ZoneState& s) { s.manifest["name"] = "A"; });
    doc.undo();
    CHECK(doc.canRedo());
    doc.edit([](ZoneState& s) { s.manifest["name"] = "B"; });
    CHECK(!doc.canRedo() && doc.history().size() == 2 && doc.state.manifest["name"] == "B");
    doc.undo();
    CHECK(doc.state.manifest["name"] != "A" && doc.state.manifest["name"] != "B");
    doc.redo();
    CHECK(doc.state.manifest["name"] == "B");

    // Save point: clean after save, dirty after an edit, clean when undone to it, dirty again on redo.
    CHECK(save_map(p, doc, false).empty() && !doc.dirty());
    doc.edit([&](ZoneState& s) { dab(s, 256); });
    CHECK(doc.dirty());
    doc.undo();
    CHECK(!doc.dirty());
    doc.redo();
    CHECK(doc.dirty());

    // Per map: another map's steps are its own, and a copied (reloaded, new) doc starts with none.
    std::string err;
    auto other = load_map(p, "meadow", err);
    CHECK(other.has_value() && !other->canUndo());
    if (other) {
        const Json name = other->state.manifest["name"];
        other->edit([](ZoneState& s) { s.manifest["name"] = "Other"; });
        const std::string mine = serialize(doc.state);
        const size_t mySteps = doc.history().size();
        other->undo();
        CHECK(other->state.manifest["name"] == name && serialize(doc.state) == mine && doc.history().size() == mySteps);
        const ZoneDoc copy = *other;
        CHECK(!copy.canUndo() && !copy.canRedo() && other->canRedo());
    }

    // The cap: 105 edits keep the last 100; undoing them all lands on the state after the 5th.
    std::string afterFifth;
    for (int i = 1; i <= 105; ++i) {
        capped.edit([i](ZoneState& s) { s.manifest["name"] = "Edit " + std::to_string(i); });
        if (i == 5) afterFifth = serialize(capped.state);
    }
    CHECK(capped.history().size() == UndoStack::kMaxSteps);
    while (capped.canUndo()) capped.undo();
    CHECK(serialize(capped.state) == afterFifth && capped.state.manifest["name"] == "Edit 5");
}

} // namespace

// Same heights: the same tiles with equal samples.
bool same_heights(const ZoneState& a, const ZoneState& b) {
    if (a.heights.size() != b.heights.size()) return false;
    for (const auto& [t, h] : a.heights) {
        const auto it = b.heights.find(t);
        if (it == b.heights.end() || *it->second != *h) return false;
    }
    return true;
}

// A raise dab at map (x, z) as one undo step; true if it changed a tile.
bool sculpt_at(ZoneDoc& doc, float x, float z) {
    std::string err;
    const auto info = doc.info(err);
    TileSet touched;
    doc.edit([&](ZoneState& s) { touched = apply_brush(*info, s, x, z, Brush{}, 10.0f); });
    return !touched.empty();
}

// Every declared pair of neighbours has equal samples on its shared edge.
bool seamless(const km::MapInfo& info, const ZoneState& state) {
    constexpr int n = km::kTileSamples;
    for (TileKey t : declared_tiles(info)) {
        const km::Heights& a = tile_heights(state, t);
        if (info.has_tile({t.x + 1, t.z})) {
            const km::Heights& b = tile_heights(state, {t.x + 1, t.z});
            for (int v = 0; v < n; ++v)
                if (a[size_t(v) * n + n - 1] != b[size_t(v) * n]) return false;
        }
        if (info.has_tile({t.x, t.z + 1})) {
            const km::Heights& b = tile_heights(state, {t.x, t.z + 1});
            for (int u = 0; u < n; ++u)
                if (a[size_t(n - 1) * n + u] != b[size_t(u)]) return false;
        }
    }
    return true;
}

// Random heights, height colors, the baked texture and the generated material.
void check_terrain_gen() {
    // Gradient: interpolation, clamping, unsorted stops, the JSON round trip and invalid stops.
    const Gradient g = {{1.0f, {1, 0, 0}}, {0.0f, {0, 0, 1}}, {0.5f, {0, 1, 0}}};
    CHECK((gradient_color(g, 0.0f) == std::array<float, 3>{0, 0, 1}));
    CHECK((gradient_color(g, -1.0f) == std::array<float, 3>{0, 0, 1}));
    CHECK((gradient_color(g, 2.0f) == std::array<float, 3>{1, 0, 0}));
    const auto quarter = gradient_color(g, 0.25f);
    CHECK(std::fabs(quarter[1] - 0.5f) < 1e-6f && std::fabs(quarter[2] - 0.5f) < 1e-6f && quarter[0] == 0);
    std::string err;
    Json manifest = {{"terrainColors", gradient_json(g)}};
    const auto parsed = parse_gradient(manifest, err);
    CHECK(parsed && parsed->size() == 3 && (*parsed)[2].pos == 0.5f && (*parsed)[0].rgb[0] == 1);
    manifest["terrainColors"] = Json::parse("[[0, [1, 1, 1]]]");
    CHECK(!parse_gradient(manifest, err));
    manifest["terrainColors"] = Json::parse("[[0, [1, 1, 1]], [1, [2, 0, 0]]]");
    CHECK(!parse_gradient(manifest, err));
    CHECK(default_gradient().size() == 4 && parse_gradient(Json{{"terrainColors", gradient_json(default_gradient())}}, err));

    // terrainUv: tiled by default, "map", anything else an error.
    nlohmann::json m = nlohmann::json::parse(R"({"version": 1, "id": 1, "name": "x", "tiles": [[0, 0, 0, 0]], "spawn": {"pos": [1, 0, 1]}})");
    CHECK(km::parse_manifest(m, err) && km::parse_manifest(m, err)->terrainUv == km::TerrainUv::Tiled);
    m["terrainUv"] = "map";
    CHECK(km::parse_manifest(m, err) && km::parse_manifest(m, err)->terrainUv == km::TerrainUv::Map);
    m["terrainUv"] = "tiled";
    CHECK(km::parse_manifest(m, err) && km::parse_manifest(m, err)->terrainUv == km::TerrainUv::Tiled);
    m["terrainUv"] = "bogus";
    CHECK(!km::parse_manifest(m, err) && err.find("terrainUv") != std::string::npos);

    // Generate on a non-rectangular map (2x2 plus 2_0): the full range, no seams, deterministic, one undo step.
    const fs::path root = copy_project(kSource / "samples" / "starter-mmo", "terrain_gen");
    Project p = scan_project(root);
    auto doc = new_map(p, "gen", "Gen", next_map_id(p), 2, 2, err);
    CHECK(doc.has_value());
    if (!doc) return;
    doc->edit([&](ZoneState& s) { add_tile(*doc->info(err), s, {2, 0}); });
    const auto info = doc->info(err);
    CHECK(info && info->has_tile({2, 0}) && !info->has_tile({2, 1}));
    if (!info) return;
    const ZoneState before = doc->state;
    NoiseParams params;
    params.seed = 7;
    doc->edit([&](ZoneState& s) { generate_heights(*info, s, params); });
    CHECK(doc->state.heights.size() == 5);
    uint16_t lo = 65535, hi = 0;
    for (const auto& [t, h] : doc->state.heights)
        for (uint16_t s : *h) lo = std::min(lo, s), hi = std::max(hi, s);
    CHECK(lo == 0 && hi == 65535);
    CHECK(seamless(*info, doc->state));
    ZoneState again = before, other = before;
    generate_heights(*info, again, params);
    params.seed = 8;
    generate_heights(*info, other, params);
    bool same = true, differ = false;
    for (const auto& [t, h] : doc->state.heights) {
        same = same && *again.heights[t] == *h;
        differ = differ || *other.heights[t] != *h;
    }
    CHECK(same && differ);
    doc->undo();
    CHECK(same_heights(doc->state, before) && doc->state.manifest == before.manifest);
    doc->redo();

    // Bake: 256 px per tile over the bounding box (3x2 tiles), grey off the map (2_1), the end colors at the lowest
    // and highest ground.
    const Gradient bw = {{0.0f, {0, 0, 1}}, {1.0f, {1, 0, 0}}};
    ZoneState plateau = before;
    auto h = std::make_shared<km::Heights>(km::flat_heights());
    for (int v = 10; v <= 30; ++v)
        for (int u = 10; u <= 30; ++u) (*h)[size_t(v) * km::kTileSamples + size_t(u)] = 65535;
    plateau.heights[{0, 0}] = h;
    const TerrainImage img = bake_terrain(*info, plateau, bw);
    CHECK(img.width == 768 && img.height == 512 && img.rgb.size() == size_t(768 * 512 * 3));
    const auto px = [&](int i, int j) {
        const uint8_t* c = &img.rgb[(size_t(j) * img.width + size_t(i)) * 3];
        return std::array<int, 3>{c[0], c[1], c[2]};
    };
    CHECK((px(20, 20) == std::array<int, 3>{255, 0, 0}));
    CHECK((px(200, 200) == std::array<int, 3>{0, 0, 255}));
    CHECK((px(600, 400) == std::array<int, 3>{128, 128, 128})); // tile 2_1: not on the map
    // The 4096 cap: 20 x 1 tiles (5120 m) shrink to 4096 x 204.
    Json wide = doc->state.manifest;
    wide["tiles"] = Json::parse("[[0, 0, 19, 0]]");
    const auto wideInfo = km::parse_manifest(nlohmann::json(wide), err);
    CHECK(wideInfo.has_value());
    if (wideInfo) {
        const TerrainImage capped = bake_terrain(*wideInfo, ZoneState{}, bw);
        CHECK(capped.width == kMaxTerrainImage && capped.height == 204);
    }

    // Create + save: the texture and material written, other materials untouched; a sculpt re-bakes on save.
    std::map<fs::path, std::string> others;
    for (const fs::path& f : {root / "materials", root / "textures"})
        if (fs::exists(f))
            for (const auto& e : fs::recursive_directory_iterator(f))
                if (e.is_regular_file()) others[e.path()] = bytes(e.path());
    CHECK(!others.empty());
    doc->edit([&](ZoneState& s) {
        s.manifest["terrainColors"] = gradient_json(bw);
        s.manifest["terrainUv"] = "map";
        s.manifest["material"] = terrain_material_path("gen");
    });
    auto errors = save_map(p, *doc, false);
    print_errors(errors);
    CHECK(errors.empty());
    const fs::path png = root / "textures" / "gen_terrain.png", mat = root / "materials" / "gen_terrain.mat";
    const auto image = kast::decodeImage(bytes(png), false);
    CHECK(image && image->width == 768 && image->height == 512);
    kast::Material loaded;
    CHECK(kast::loadMaterial(mat, loaded, err) && loaded.baseColorTexture == "textures/gen_terrain.png" &&
          loaded.metallic == 0 && loaded.roughness == 0.9f && loaded.baseColor[0] == 1);
    for (const auto& [f, data] : others) CHECK(bytes(f) == data);
    const auto reloaded = load_map(p, "gen", err);
    CHECK(reloaded && reloaded->info(err)->terrainUv == km::TerrainUv::Map && parse_gradient(reloaded->state.manifest, err));
    const std::string firstPng = bytes(png);
    CHECK(sculpt_at(*doc, 300, 200));
    CHECK(save_map(p, *doc, false).empty() && bytes(png) != firstPng);

    // A file already at the generated path that this map did not write: the save is blocked and writes nothing.
    auto other2 = new_map(p, "gen2", "Gen2", next_map_id(p), 1, 1, err);
    CHECK(other2.has_value());
    if (!other2) return;
    kast::writeFileAtomic(root / "materials" / "gen2_terrain.mat", "hand made");
    other2->edit([&](ZoneState& s) {
        s.manifest["terrainColors"] = gradient_json(bw);
        s.manifest["terrainUv"] = "map";
        s.manifest["material"] = terrain_material_path("gen2");
    });
    errors = save_map(p, *other2, false);
    CHECK(errors.size() == 1 && errors[0].file == "materials/gen2_terrain.mat");
    CHECK(bytes(root / "materials" / "gen2_terrain.mat") == "hand made" && !fs::exists(root / "textures" / "gen2_terrain.png") &&
          !fs::exists(root / "maps" / "gen2"));
}

// Tile scenes (engine/tile_scenes.h, the Map tab and Play's streaming): paths, cell states, out-of-bounds roots, the
// streaming diff, event payloads, the thumbnail colors, and a tile removed in the Zone tab keeping its scene (an orphan).
// The editor's Scene view: meshes for the tiles around the camera's pivot, and a manifest edit rebuilds them only when
// what the renderer reads from the map changed.
void check_edit_view() {
    km::MapInfo big;
    big.tiles = {{0, 0, kMaxMapTiles - 1, kMaxMapTiles - 1}};
    const float mid = kMaxMapTiles / 2 * km::kTileSize + 10;
    const auto here = kast::tiles_around(big, {mid, mid}, kEditRadius);
    const int side = 2 * kEditRadius + 1;
    CHECK(here.size() == size_t(side * side));
    // One tile east: a column goes, a column comes, the rest stay.
    const auto east = kast::tiles_around(big, {mid + km::kTileSize, mid}, kEditRadius);
    const auto kept = std::ranges::count_if(east, [&](km::TileKey t) { return std::ranges::find(here, t) != here.end(); });
    CHECK(east.size() == here.size() && kept == side * (side - 1));
    CHECK(kast::tiles_around(big, {10, 10}, kEditRadius).size() == size_t((kEditRadius + 1) * (kEditRadius + 1)));
    CHECK(kast::tiles_around(big, {-5000, -5000}, kEditRadius).empty());
    km::MapInfo eight;
    eight.tiles = {{0, 0, 7, 7}};
    CHECK(kast::tiles_around(eight, {1024, 1024}, kEditRadius).size() == 64); // framed: all of an 8 x 8 map

    km::MapInfo moved = big;
    moved.spawn = {500, 0, 500};
    moved.name = "renamed";
    moved.tiles.push_back({kMaxMapTiles, 0, kMaxMapTiles, 0}); // a tile added at the edge, Tiled UVs
    CHECK(same_terrain_mesh(big, moved));
    for (const auto change : std::initializer_list<void (*)(km::MapInfo&)>{
             [](km::MapInfo& m) { m.heightRange = std::array<float, 2>{0, 500}; },
             [](km::MapInfo& m) { m.material = "materials/rock.mat"; },
             [](km::MapInfo& m) { m.terrainUv = km::TerrainUv::Map; },
             [](km::MapInfo& m) { m.id = 99; }}) {
        km::MapInfo other = big;
        change(other);
        CHECK(!same_terrain_mesh(big, other));
    }
    km::MapInfo mapUv = big, grown;
    mapUv.terrainUv = km::TerrainUv::Map;
    grown = mapUv;
    grown.tiles.push_back({kMaxMapTiles, 0, kMaxMapTiles, 0}); // Map UVs: a bigger bounding box moves every UV
    CHECK(!same_terrain_mesh(mapUv, grown));
    grown = mapUv;
    grown.spawn = {1, 0, 1};
    CHECK(same_terrain_mesh(mapUv, grown));
}

void check_tile_scenes() {
    using kast::TileCell;
    CHECK(kast::tileScenePath("tilegrid", {1, 2}) == "maps/tilegrid/scenes/1_2.scene");
    CHECK(kast::tileSceneTile("3_-1.scene") == km::TileKey(3, -1) && kast::tileSceneTile("-2_0.scene") == km::TileKey(-2, 0));
    for (const char* bad : {"1_2.scen", "1-2.scene", "a_2.scene", "1_2_3.scene", "_2.scene", "1_.scene", "+1_2.scene",
                            "1_2.scene.meta", "notes.txt", "1 _2.scene"})
        CHECK(!kast::tileSceneTile(bad));
    const auto ref = kast::parseTileScenePath("maps/tilegrid/scenes/2_0.scene");
    CHECK(ref && ref->folder == "tilegrid" && ref->tile == km::TileKey(2, 0));
    CHECK(kast::parseTileScenePath("maps\\tilegrid\\scenes\\2_0.scene").has_value());
    for (const char* bad : {"scenes/tilegrid.scene", "maps/tilegrid/2_0.scene", "maps//scenes/2_0.scene",
                            "maps/tilegrid/scenes/x/2_0.scene", "x/maps/tilegrid/scenes/2_0.scene"})
        CHECK(!kast::parseTileScenePath(bad));

    CHECK(kast::tileCell(false, false, false, false) == TileCell::Undeclared);
    CHECK(kast::tileCell(false, true, false, false) == TileCell::Orphan);
    CHECK(kast::tileCell(false, true, true, true) == TileCell::Orphan); // undeclared wins
    CHECK(kast::tileCell(true, false, false, false) == TileCell::Declared);
    CHECK(kast::tileCell(true, true, false, false) == TileCell::HasScene);
    CHECK(kast::tileCell(true, true, true, false) == TileCell::Open);
    CHECK(kast::tileCell(true, true, true, true) == TileCell::Unsaved);
    CHECK(kast::tileCell(true, false, true, false) == TileCell::Unsaved); // created, not saved yet

    CHECK(!kast::outsideTile({1, 0}, 256, 0) && !kast::outsideTile({1, 0}, 511.9f, 255.9f));
    CHECK(kast::outsideTile({1, 0}, 512, 10) == km::TileKey(2, 0) && kast::outsideTile({0, 0}, -0.5f, 3) == km::TileKey(-1, 0));

    // Streaming: tile 1_1 wanted (radius 0 at its centre); 0_0 and 1_0 are loaded, so are another map's tile scene, a
    // plain scene and the active root.
    const std::vector<km::TileKey> withScene{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const std::vector<std::string> loaded{"scenes/tilegrid.scene", "maps/tilegrid/scenes/0_0.scene",
                                          "maps/tilegrid/scenes/1_0.scene", "maps/other/scenes/1_1.scene", "levels/x.scene"};
    auto plan = kast::planTileScenes("tilegrid", {{1, 1}}, withScene, loaded, "scenes/tilegrid.scene", {});
    CHECK((plan.unload == std::vector<std::string>{"maps/tilegrid/scenes/0_0.scene", "maps/tilegrid/scenes/1_0.scene",
                                                   "maps/other/scenes/1_1.scene"}));
    CHECK(plan.load == "maps/tilegrid/scenes/1_1.scene");
    plan = kast::planTileScenes("tilegrid", {{1, 1}}, withScene, loaded, "scenes/tilegrid.scene",
                                {"maps/tilegrid/scenes/1_1.scene"});
    CHECK(!plan.load); // a failed load is not retried
    // Radius 1 around 0_0: 0_0 and 1_0 stay; one load a frame, in wanted order (z then x), none twice.
    km::MapInfo grid;
    grid.tiles = {{0, 0, 2, 2}};
    const std::vector<km::TileKey> around = kast::tiles_around(grid, {10, 10}, 1);
    CHECK((around == std::vector<km::TileKey>{{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    plan = kast::planTileScenes("tilegrid", around, withScene, loaded, "scenes/tilegrid.scene", {});
    CHECK((plan.unload == std::vector<std::string>{"maps/other/scenes/1_1.scene"}));
    CHECK(plan.load == "maps/tilegrid/scenes/0_1.scene");
    // The active scene is never unloaded, even a tile scene.
    plan = kast::planTileScenes("tilegrid", {}, withScene, {"maps/tilegrid/scenes/0_0.scene"},
                                "maps/tilegrid/scenes/0_0.scene", {});
    CHECK(plan.unload.empty() && !plan.load);

    const nlohmann::json event = kast::tileSceneEvent(7, {"tilegrid", {2, 1}});
    CHECK(event == nlohmann::json({{"map", 7}, {"tx", 2}, {"tz", 1}, {"path", "maps/tilegrid/scenes/2_1.scene"}}));

    std::string err;
    auto summary = kast::summarizeTileScene(R"({"entities": [
        {"id": 1, "name": "A", "parent": null, "components": {"Transform": {"position": [600, 1, 400]}}},
        {"id": 2, "name": "B", "parent": 1, "components": {}},
        {"id": 3, "name": "C", "parent": null, "components": 5}]})", err);
    CHECK(summary && summary->entities == 3 && summary->roots.size() == 2 && summary->roots[0].name == "A" &&
          summary->roots[0].xz == (std::array<float, 2>{600, 400}) && summary->roots[1].xz == (std::array<float, 2>{0, 0}));
    CHECK(!kast::summarizeTileScene("{", err) && !err.empty());
    CHECK(!kast::summarizeTileScene(R"({"entities": 1})", err));

    // Thumbnails: grayscale without terrainColors, the gradient's colors with; row 0 is the tile's max z.
    ZoneState state;
    auto heights = std::make_shared<km::Heights>(km::flat_heights());
    (*heights)[size_t(km::kTileSamples - 1) * km::kTileSamples] = 65535; // u 0, v 256: the north-west corner
    state.heights[{0, 0}] = heights;
    TerrainImage gray = tile_thumbnail(state, {0, 0}, std::nullopt, 8);
    CHECK(gray.width == 8 && gray.height == 8 && gray.rgb.size() == 8 * 8 * 3);
    CHECK(gray.rgb[0] == 255 && gray.rgb[1] == 255 && gray.rgb[2] == 255); // top-left: high
    CHECK(gray.rgb[(7 * 8 + 7) * 3] == 0 && gray.rgb[(7 * 8) * 3] == 0);  // the south row: flat, low
    const Gradient two{{0, {1, 0, 0}}, {1, {0, 0, 1}}};
    TerrainImage tinted = tile_thumbnail(state, {0, 0}, two, 8);
    CHECK(tinted.rgb[0] == 0 && tinted.rgb[2] == 255 && tinted.rgb[(7 * 8) * 3] == 255 && tinted.rgb[(7 * 8) * 3 + 2] == 0);
    CHECK(tile_thumbnail(state, {5, 5}, std::nullopt, 4).rgb == std::vector<uint8_t>(4 * 4 * 3, 0)); // no heights: flat

    // The test map: 9 tiles, scenes on 4 of them plus the orphan 3_1. Removing 1_1 in the zone keeps its scene.
    const fs::path root = copy_project(kSource / "tests" / "project", "tile_scenes");
    const fs::path scenes = root / "maps" / "tilegrid" / "scenes";
    fs::create_directories(scenes / "sub");
    std::ofstream(scenes / "notes.txt") << "x";
    std::ofstream(scenes / "sub" / "0_0.scene") << "{}";
    const kast::TileSceneFiles files = kast::listTileScenes(root / "maps" / "tilegrid");
    CHECK((files.tiles == std::vector<km::TileKey>{{0, 0}, {1, 0}, {0, 1}, {1, 1}, {3, 1}}));
    CHECK((files.badNames == std::vector<std::string>{"notes.txt"})); // .meta files and subfolders are left out
    Project p = scan_project(root);
    auto doc = load_map(p, "tilegrid", err);
    CHECK(doc.has_value() && validate(p, *doc).empty());
    if (!doc) return;
    auto info = doc->info(err);
    CHECK(info && declared_tiles(*info).size() == 9 && !info->has_tile({3, 1}));
    if (!info) return;
    doc->edit([&](ZoneState& s) { remove_tile(*info, s, {1, 1}); });
    CHECK(save_map(p, *doc, false).empty());
    CHECK(fs::exists(scenes / "1_1.scene") && !fs::exists(root / "maps" / "tilegrid" / "heights" / "1_1.png") &&
          !fs::exists(root / "maps" / "tilegrid" / "heights" / "1_1.r16"));
    info = doc->info(err);
    CHECK(info && kast::tileCell(info->has_tile({1, 1}), true, false, false) == TileCell::Orphan);
}

int main(int argc, char** argv) {
    kast::disableCrashDialogsIfRequested();
    if (argc == 3 && std::string(argv[1]) == "--child") { // check_process_runner's child: stdout, stderr, exit code
        std::printf("child: %s\n", argv[2]);
        std::fflush(stdout);
        std::fprintf(stderr, "child: stderr\n");
        return 3;
    }
    check_round_trip(kSource / "samples" / "starter-mmo", "rt_meadow", "meadow");
    check_round_trip(kSource / "samples" / "starter-mmo", "rt_cavern", "cavern");
    check_round_trip(kSource / "tests" / "project", "rt_hills", "hills");
    check_round_trip(kSource / "tests" / "project", "rt_crypt", "crypt");
    check_instance();
    check_new_map();
    check_undo();
    check_raycast();
    check_brushes();
    check_height_range();
    check_png();
    check_areas();
    check_spawns();
    check_objects();
    check_asset_refs();
    check_scatter();
    check_undo_each();
    check_undo_history();
    check_tiles();
    check_process_runner();
    check_terrain_gen();
    check_round_trip(kSource / "tests" / "project", "rt_tilegrid", "tilegrid");
    check_tile_scenes();
    check_edit_view();
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("zone builder self-check: all passed\n");
    return failures ? 1 : 0;
}
