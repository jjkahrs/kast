#include "tile_scenes.h"

#include "zone_scenes.h"

#include "files.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace kast {
namespace {

std::optional<int32_t> parseInt(std::string_view text)
{
    int32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || text.empty() || text.front() == '+')
        return std::nullopt;
    return value;
}

bool tileLess(map::TileKey a, map::TileKey b) { return a.z != b.z ? a.z < b.z : a.x < b.x; }

} // namespace

std::string tileScenePath(const std::string& folder, map::TileKey tile)
{
    return std::format("maps/{}/scenes/{}_{}.scene", folder, tile.x, tile.z);
}

std::optional<map::TileKey> tileSceneTile(std::string_view fileName)
{
    constexpr std::string_view kExtension = ".scene";
    if (!fileName.ends_with(kExtension))
        return std::nullopt;
    fileName.remove_suffix(kExtension.size());
    const size_t split = fileName.find('_', 1); // past a leading '-'
    if (split == std::string_view::npos)
        return std::nullopt;
    const std::optional<int32_t> x = parseInt(fileName.substr(0, split));
    const std::optional<int32_t> z = parseInt(fileName.substr(split + 1));
    if (!x || !z)
        return std::nullopt;
    return map::TileKey{*x, *z};
}

std::optional<TileSceneRef> parseTileScenePath(std::string_view path)
{
    std::vector<std::string_view> parts;
    for (size_t start = 0; start <= path.size();) {
        const size_t end = std::min(path.find_first_of("/\\", start), path.size());
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    if (parts.size() != 4 || parts[0] != "maps" || parts[1].empty() || parts[2] != "scenes")
        return std::nullopt;
    const std::optional<map::TileKey> tile = tileSceneTile(parts[3]);
    if (!tile)
        return std::nullopt;
    return TileSceneRef{std::string(parts[1]), *tile};
}

TileSceneFiles listTileScenes(const std::filesystem::path& mapFolder)
{
    TileSceneFiles files;
    const std::filesystem::path dir = (mapFolder / "scenes").lexically_normal();
    for (const std::filesystem::path& file : listFiles(dir)) {
        const std::u8string name = file.filename().u8string();
        const std::string text(name.begin(), name.end());
        if (file.parent_path().lexically_normal() != dir || text.ends_with(".meta"))
            continue; // a subfolder's files aren't tile scenes; .meta: the editor's asset ids
        if (const std::optional<map::TileKey> tile = tileSceneTile(text))
            files.tiles.push_back(*tile);
        else
            files.badNames.push_back(text);
    }
    std::ranges::sort(files.tiles, tileLess);
    return files;
}

TileScenePlan planTileScenes(const std::string& folder, const std::vector<map::TileKey>& wanted,
                             const std::vector<map::TileKey>& withScene, const std::vector<std::string>& loaded,
                             const std::string& active, const std::set<std::string>& skip,
                             const std::vector<std::string>& wantedZones, const std::vector<std::string>& zonesWithScene)
{
    TileScenePlan plan;
    for (const std::string& path : loaded) {
        if (path == active)
            continue;
        if (const std::optional<TileSceneRef> ref = parseTileScenePath(path);
            ref && (ref->folder != folder || std::ranges::find(wanted, ref->tile) == wanted.end()))
            plan.unload.push_back(path);
        if (const std::optional<ZoneSceneRef> ref = parseZoneScenePath(path);
            ref && (ref->folder != folder || std::ranges::find(wantedZones, ref->zone) == wantedZones.end()))
            plan.unload.push_back(path);
    }
    const auto loadable = [&](const std::string& path) {
        return std::ranges::find(loaded, path) == loaded.end() && !skip.contains(path);
    };
    for (const map::TileKey tile : wanted) {
        if (std::ranges::find(withScene, tile) != withScene.end() && loadable(tileScenePath(folder, tile))) {
            plan.load = tileScenePath(folder, tile);
            return plan;
        }
    }
    for (const std::string& zone : wantedZones) {
        if (std::ranges::find(zonesWithScene, zone) != zonesWithScene.end() && loadable(zoneScenePath(folder, zone))) {
            plan.load = zoneScenePath(folder, zone);
            return plan;
        }
    }
    return plan;
}

nlohmann::json tileSceneEvent(uint16_t mapId, const TileSceneRef& ref)
{
    return {{"map", mapId}, {"tx", ref.tile.x}, {"tz", ref.tile.z}, {"path", tileScenePath(ref.folder, ref.tile)}};
}

TileCell tileCell(bool declared, bool hasFile, bool open, bool unsaved)
{
    if (!declared)
        return hasFile ? TileCell::Orphan : TileCell::Undeclared;
    if (open)
        return unsaved || !hasFile ? TileCell::Unsaved : TileCell::Open;
    return hasFile ? TileCell::HasScene : TileCell::Declared;
}

const char* tileCellName(TileCell cell)
{
    switch (cell) {
    case TileCell::Undeclared: return "not a map tile";
    case TileCell::Declared: return "no scene";
    case TileCell::HasScene: return "has a scene";
    case TileCell::Open: return "open";
    case TileCell::Unsaved: return "open, unsaved";
    case TileCell::Orphan: return "orphan: scene on an undeclared tile";
    }
    return "";
}

std::optional<map::TileKey> outsideTile(map::TileKey tile, float x, float z)
{
    const map::TileKey at = map::tile_of(x, z);
    return at == tile ? std::nullopt : std::optional(at);
}

std::optional<TileSceneSummary> summarizeTileScene(std::string_view text, std::string& error)
{
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("entities") || !json["entities"].is_array()) {
        error = "not a scene file (no \"entities\" array)";
        return std::nullopt;
    }
    TileSceneSummary summary;
    summary.entities = json["entities"].size();
    for (const nlohmann::json& entity : json["entities"]) {
        if (!entity.is_object() || !entity.value("parent", nlohmann::json()).is_null())
            continue;
        TileSceneRoot root{entity.contains("name") && entity["name"].is_string() ? entity["name"].get<std::string>() : ""};
        const nlohmann::json* position = &entity;
        for (const char* key : {"components", "Transform", "position"})
            position = position && position->is_object() && position->contains(key) ? &(*position)[key] : nullptr;
        if (position && position->is_array() && position->size() == 3 && (*position)[0].is_number() &&
            (*position)[2].is_number())
            root.xz = {(*position)[0].get<float>(), (*position)[2].get<float>()};
        summary.roots.push_back(std::move(root));
    }
    return summary;
}

} // namespace kast
