#pragma once

#include "event_bus.h"
#include "scene.h"

#include <heights.h>
#include <map.h>
#include <objects.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Heightmap terrain (docs/engine/terrain.html): the project's maps (maps/<folder>/map.json, areas.json,
// heights/<tx>_<tz>.r16|.png, see heights.h, and objects/<tx>_<tz>.json, see objects.h) streamed as TerrainTile entities
// around the local player in Play, each tile's objects as its children. The renderer and physics react to those
// entities; neither reads files nor knows about maps.
namespace kast {

struct PrefabAsset;

// What a TerrainTile renders and collides with.
struct TerrainTileData {
    std::shared_ptr<const map::MapInfo> map; // heightRange, material, nav
    map::TileKey tile;
    map::Heights heights;
    std::vector<map::ZoneObject> objects; // the tile's objects file, its bad entries left out
};

// The declared tiles of info within radius tiles (a square) of the tile holding centre (x, z), sorted by z then x.
std::vector<map::TileKey> tiles_around(const map::MapInfo& info, std::array<float, 2> centre, int radius);

// Prefabs by project-relative path, each loaded once; a failed load is remembered with its error.
struct PrefabCache {
    std::filesystem::path projectRoot;
    std::map<std::string, std::pair<std::shared_ptr<const PrefabAsset>, std::string>> loaded;

    // The prefab, or null with error set. fresh: this call tried the load (so a failure is reported once).
    std::shared_ptr<const PrefabAsset> get(const std::string& path, std::string& error, bool* fresh = nullptr);
};

// A zone object under parent, whose world position is origin (a tile's corner): an entity at the object's pos and
// yaw with, for an obstacle or interactable, a static Collider from its shape (standing on pos, not scaled), and the
// prefab instance as its child, scaled by the object's scale. entt::null + error if the prefab doesn't load.
entt::entity spawnZoneObject(Scene& scene, entt::entity parent, const bx::Vec3& origin, const map::ZoneObject& object,
                             PrefabCache& prefabs, std::string& error);

// The interactable nearest to (x, z) by the horizontal distance to its shape's edge (map::footprint_distance), among the
// scene's streamed tiles, if one is within maxDistance.
struct NearbyInteractable {
    const map::ZoneObject* object;
    float distance;
};
std::optional<NearbyInteractable> nearestInteractable(const Scene& scene, float x, float z, float maxDistance);

// Owned next to physics by LuaSystems for one Play session.
class TerrainStreamer {
public:
    // Loads every map manifest (and areas.json) under projectRoot/maps; forgets the current area. No project: none.
    void start(const std::optional<std::filesystem::path>& projectRoot);
    // Makes the scene's TerrainTile entities (children of the first Terrain entity) the declared tiles of
    // Terrain.map within Terrain.radius of the centre: the NetworkLocalPlayer, else the active camera, else the
    // origin. Heights and objects load on the frame a tile appears (its objects as its children, spawnZoneObject; a bad
    // entry or prefab is logged with the file and skipped), and go with it. Then publishes area.left / area.entered
    // {map, id, name} on bus when the centre's area (map::area_at) changed.
    void update(Scene& scene, EventBus& bus);

    // A loaded map, or null.
    const map::MapInfo* map(uint16_t id) const;

private:
    struct LoadedMap {
        std::shared_ptr<const map::MapInfo> info;
        std::filesystem::path folder;
    };
    std::shared_ptr<const TerrainTileData> load(const LoadedMap& map, map::TileKey tile) const;
    void spawnObjects(Scene& scene, entt::entity tile, const LoadedMap& map, const TerrainTileData& data);
    void setArea(EventBus& bus, uint16_t map, int16_t area);

    std::map<uint16_t, LoadedMap> maps_;
    PrefabCache prefabs_; // the zone objects' prefabs, for the session
    uint16_t areaMap_ = 0;
    int16_t area_ = -1; // index into maps_[areaMap_].info->areas.list
    int warnedMap_ = -1; // the unknown Terrain.map last warned about
};

} // namespace kast
