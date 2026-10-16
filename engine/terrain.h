#pragma once

#include "event_bus.h"
#include "scene.h"

#include <heights.h>
#include <map.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// Heightmap terrain (README "Terrain"): the project's maps (maps/<folder>/map.json, areas.json and
// heights/<tx>_<tz>.r16|.png, see heights.h) streamed as TerrainTile entities around the local player in Play. The
// renderer and physics react to those entities; neither reads files nor knows about maps.
namespace kast {

// What a TerrainTile renders and collides with.
struct TerrainTileData {
    std::shared_ptr<const map::MapInfo> map; // heightRange, material, nav
    map::TileKey tile;
    map::Heights heights;
};

// The declared tiles of info within radius tiles (a square) of the tile holding centre (x, z), sorted by z then x.
std::vector<map::TileKey> tiles_around(const map::MapInfo& info, std::array<float, 2> centre, int radius);

// Owned next to physics by LuaSystems for one Play session.
class TerrainStreamer {
public:
    // Loads every map manifest (and areas.json) under projectRoot/maps; forgets the current area. No project: none.
    void start(const std::optional<std::filesystem::path>& projectRoot);
    // Makes the scene's TerrainTile entities (children of the first Terrain entity) the declared tiles of
    // Terrain.map within Terrain.radius of the centre: the NetworkLocalPlayer, else the active camera, else the
    // origin. Heights load on the frame a tile appears. Then publishes area.left / area.entered {map, id, name} on bus
    // when the centre's area (map::area_at) changed.
    void update(Scene& scene, EventBus& bus);

    // A loaded map, or null.
    const map::MapInfo* map(uint16_t id) const;

private:
    struct LoadedMap {
        std::shared_ptr<const map::MapInfo> info;
        std::filesystem::path folder;
    };
    std::shared_ptr<const TerrainTileData> load(const LoadedMap& map, map::TileKey tile) const;
    void setArea(EventBus& bus, uint16_t map, int16_t area);

    std::map<uint16_t, LoadedMap> maps_;
    uint16_t areaMap_ = 0;
    int16_t area_ = -1; // index into maps_[areaMap_].info->areas.list
    int warnedMap_ = -1; // the unknown Terrain.map last warned about
};

} // namespace kast
