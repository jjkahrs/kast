// kast_mapbake <mapDir> [--out <dir>]: lints a map folder (height seams, spawn files) and bakes a navtile per declared
// tile into <mapDir>/.server/nav (or --out). Exit 0: all baked; 1: lint or bake errors (nothing written); 2: usage.
#include "bake.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace kast;

namespace {

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string tile_name(map::TileKey t) { return std::to_string(t.x) + "_" + std::to_string(t.z); }

// Every declared tile's heights file (.r16 first, as the server and the engine), decoded.
map::TileHeights load_heights(const fs::path& dir, const map::MapInfo& info, std::vector<std::string>& errors) {
    map::TileHeights out;
    for (const auto& r : info.tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx)
                for (const char* ext : {".r16", ".png"}) {
                    const fs::path file = dir / "heights" / (tile_name({tx, tz}) + ext);
                    const auto bytes = read_file(file);
                    if (!bytes) continue;
                    std::string err;
                    if (auto h = map::decode_heights(*bytes, ext, err)) out[{tx, tz}] = std::move(*h);
                    else errors.push_back(file.string() + ": " + err);
                    break;
                }
    return out;
}

// The spawn files, checked as the server loads them (map::parse_spawns, tile on the map, ids unique on the map).
void lint_spawns(const fs::path& dir, const map::MapInfo& info, std::vector<std::string>& errors) {
    std::error_code ec;
    std::set<std::string> ids;
    for (const auto& entry : fs::directory_iterator(dir / ".server" / "spawns", ec)) {
        const std::string where = entry.path().string();
        static const std::regex re(R"((-?\d{1,9})_(-?\d{1,9})\.json)");
        std::smatch m;
        const std::string file = entry.path().filename().string();
        if (!std::regex_match(file, m, re)) { errors.push_back(where + ": file name must be <tx>_<tz>.json"); continue; }
        const map::TileKey t{std::stoi(m[1]), std::stoi(m[2])};
        if (!info.has_tile(t)) { errors.push_back(where + ": tile is not on the map"); continue; }
        std::string err;
        const auto json = nlohmann::json::parse(read_file(entry.path()).value_or(""), nullptr, false);
        const auto spawns = json.is_discarded() ? std::nullopt : map::parse_spawns(json, t, err);
        if (!spawns) { errors.push_back(where + ": " + (json.is_discarded() ? "not valid JSON" : err)); continue; }
        for (const auto& s : *spawns)
            if (!ids.insert(s.id).second) errors.push_back(where + ": spawn \"" + s.id + "\": duplicate id on this map");
    }
}

int run(const fs::path& dir, fs::path out) {
    std::vector<std::string> errors;
    std::string err;
    const auto text = read_file(dir / "map.json");
    const auto json = nlohmann::json::parse(text.value_or(""), nullptr, false);
    const auto info = json.is_discarded() ? std::nullopt : map::parse_manifest(json, err);
    if (!info) {
        std::fprintf(stderr, "%s: %s\n", (dir / "map.json").string().c_str(), text ? (err.empty() ? "not valid JSON" : err.c_str()) : "missing");
        return 1;
    }
    const map::TileHeights heights = load_heights(dir, *info, errors);
    for (auto& e : mapbake::seam_errors(*info, heights)) errors.push_back((dir / "heights").string() + ": " + e);
    lint_spawns(dir, *info, errors);
    if (!errors.empty()) {
        for (const auto& e : errors) std::fprintf(stderr, "error: %s\n", e.c_str());
        std::fprintf(stderr, "%zu error(s), nothing baked\n", errors.size());
        return 1;
    }

    if (out.empty()) out = dir / ".server" / "nav";
    std::vector<std::pair<std::string, std::string>> files; // written only once every tile baked
    for (const auto& r : info->tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx) {
                const auto t0 = std::chrono::steady_clock::now();
                const auto tile = mapbake::bake_tile(*info, heights, {tx, tz}, err);
                const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const std::string name = tile_name({tx, tz}) + ".navtile";
                if (!tile) { std::fprintf(stderr, "error: %s: %s\n", name.c_str(), err.c_str()); return 1; }
                std::printf("%s: %d polys, %zu bytes, %.2f s%s\n", name.c_str(), tile->polys, tile->navtile.size(), sec,
                            tile->navtile.empty() ? " (nothing walkable: no file)" : "");
                if (!tile->navtile.empty()) files.emplace_back(name, tile->navtile);
            }
    std::error_code ec;
    fs::create_directories(out, ec);
    for (const auto& [name, bytes] : files) {
        std::ofstream f(out / name, std::ios::binary | std::ios::trunc);
        if (!f.write(bytes.data(), std::streamsize(bytes.size()))) {
            std::fprintf(stderr, "error: cannot write %s\n", (out / name).string().c_str());
            return 1;
        }
    }
    std::printf("%zu navtile(s) -> %s\n", files.size(), out.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    fs::path dir, out;
    bool usage = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (dir.empty() && !a.starts_with("--")) dir = a;
        else usage = true;
    }
    if (usage || dir.empty()) {
        std::fprintf(stderr, "usage: kast_mapbake <mapDir> [--out <dir>]\n");
        return 2;
    }
    return run(dir, out);
}
