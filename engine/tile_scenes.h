#pragma once

#include <map.h>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Tile scenes (docs/engine/terrain.html#tile-scenes, docs/editor/map-builder.html): each 256 m tile of a map may have
// its own scene file, maps/<folder>/scenes/<tx>_<tz>.scene (project-relative; the file exists <=> the tile has one),
// its entities in world coordinates. The zone's root scene (the one with the Terrain entity) stays the active scene; tile
// scenes load additively: opened from the editor's Map tab, streamed around the player in Play (LuaSystems). Pure: the
// paths, the cell states and the streaming diff, no window and no Scene.
namespace kast {

// "maps/<folder>/scenes/<tx>_<tz>.scene"
std::string tileScenePath(const std::string& folder, map::TileKey tile);
// The tile a "<tx>_<tz>.scene" file name names (decimal integers, '-' allowed); nullopt for any other name.
std::optional<map::TileKey> tileSceneTile(std::string_view fileName);

struct TileSceneRef {
    std::string folder; // maps/<folder>
    map::TileKey tile;
};
// A project-relative "maps/<folder>/scenes/<tx>_<tz>.scene" ('/' or '\'); nullopt for any other path.
std::optional<TileSceneRef> parseTileScenePath(std::string_view path);

// A map folder's scenes/: the tiles with a scene file (sorted z then x) and the names of the other files there.
struct TileSceneFiles {
    std::vector<map::TileKey> tiles;
    std::vector<std::string> badNames;
};
TileSceneFiles listTileScenes(const std::filesystem::path& mapFolder);

// One frame of tile scene streaming: unload every loaded tile scene (of any map) that is not one of folder's wanted
// tiles, except the active scene; load the first wanted tile (in wanted order) that has a scene file, isn't loaded
// and isn't in skip (failed loads).
struct TileScenePlan {
    std::vector<std::string> unload; // project-relative paths
    std::optional<std::string> load;
};
TileScenePlan planTileScenes(const std::string& folder, const std::vector<map::TileKey>& wanted,
                             const std::vector<map::TileKey>& withScene, const std::vector<std::string>& loaded,
                             const std::string& active, const std::set<std::string>& skip);

// The payload of map.tileScene.loaded / map.tileScene.unloaded: {map, tx, tz, path}.
nlohmann::json tileSceneEvent(uint16_t mapId, const TileSceneRef& ref);

// A Map tab cell: Orphan is a scene file on an undeclared tile; Unsaved an open tile scene with unsaved changes (or
// created and not saved yet).
enum class TileCell { Undeclared, Declared, HasScene, Open, Unsaved, Orphan };
TileCell tileCell(bool declared, bool hasFile, bool open, bool unsaved);
const char* tileCellName(TileCell cell);

// The tile (x, z) is in when that is not tile; nullopt inside it.
std::optional<map::TileKey> outsideTile(map::TileKey tile, float x, float z);

// What the Map tab reads from a tile scene file it doesn't have open: its entity count and root entities' names and
// world x/z.
struct TileSceneRoot {
    std::string name;
    std::array<float, 2> xz{};
};
struct TileSceneSummary {
    size_t entities = 0;
    std::vector<TileSceneRoot> roots;
};
// nullopt with error set if text is not a scene file's JSON with an "entities" array.
std::optional<TileSceneSummary> summarizeTileScene(std::string_view text, std::string& error);

} // namespace kast
