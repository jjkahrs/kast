#include "terrain.h"

#include "files.h"
#include "lightmap.h"
#include "log.h"
#include "prefab.h"
#include "replication.h" // yawRotation
#include "scene_io.h"   // loadPrefab

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace kast {
namespace {

using Json = nlohmann::json;

std::optional<Json> readJson(const std::filesystem::path& file, bool& exists)
{
    std::string text;
    exists = readFile(file, text);
    if (!exists)
        return std::nullopt;
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded())
        return std::nullopt;
    return j;
}

bx::Vec3 worldPosition(const Scene& scene, entt::entity entity)
{
    float m[16];
    scene.worldMatrix(entity, m);
    return {m[12], m[13], m[14]};
}

} // namespace

std::vector<map::TileKey> tiles_around(const map::MapInfo& info, std::array<float, 2> centre, int radius)
{
    const map::TileKey c = map::tile_of(centre[0], centre[1]);
    std::vector<map::TileKey> tiles;
    for (int32_t z = c.z - radius; z <= c.z + radius; ++z)
        for (int32_t x = c.x - radius; x <= c.x + radius; ++x)
            if (info.has_tile({x, z}))
                tiles.push_back({x, z});
    return tiles;
}

std::shared_ptr<const PrefabAsset> PrefabCache::get(const std::string& path, std::string& error, bool* fresh)
{
    auto [it, added] = loaded.try_emplace(path);
    if (fresh)
        *fresh = added;
    if (added && !loadPrefab(projectRoot, path, it->second.first, it->second.second))
        it->second.first.reset();
    error = it->second.second;
    return it->second.first;
}

entt::entity spawnZoneObject(Scene& scene, entt::entity parent, const bx::Vec3& origin, const map::ZoneObject& object,
                             PrefabCache& prefabs, std::string& error)
{
    const std::shared_ptr<const PrefabAsset> prefab = prefabs.get(object.prefab, error);
    if (!prefab) {
        error = std::format("prefab \"{}\": {}", object.prefab, error);
        return entt::null;
    }
    const entt::entity entity = scene.create(std::format("{} {}", map::object_type_name(object.type),
                                                         std::filesystem::path(object.prefab).stem().string()),
                                             parent);
    if (entity == entt::null) {
        error = "the hierarchy is too deep";
        return entt::null;
    }
    entt::registry& registry = scene.registry();
    Transform& transform = registry.get<Transform>(entity);
    transform.position = bx::sub(bx::Vec3{object.pos[0], object.pos[1], object.pos[2]}, origin);
    transform.rotation = yawRotation(object.yaw);
    if (object.shape) {
        Collider collider; // static: no Rigidbody
        const map::Shape& s = *object.shape;
        if (s.kind == map::Shape::Kind::Box) {
            collider.size = {2 * s.halfExtents[0], 2 * s.halfExtents[1], 2 * s.halfExtents[2]};
            collider.center = {0.0f, s.halfExtents[1], 0.0f};
        } else {
            collider.shape = ColliderShape::Cylinder;
            collider.radius = s.radius;
            collider.height = s.height;
            collider.center = {0.0f, s.height * 0.5f, 0.0f};
        }
        registry.emplace<Collider>(entity, collider);
    }
    const entt::entity instance = instantiatePrefab(scene, prefab, entity, error);
    if (instance == entt::null) {
        scene.destroy(entity);
        return entt::null;
    }
    Transform& t = registry.get<Transform>(instance); // the prefab root's own offset, scaled with it
    t.position = bx::mul(t.position, object.scale);
    t.scale = bx::mul(t.scale, object.scale);
    return entity;
}

std::optional<NearbyInteractable> nearestInteractable(const Scene& scene, float x, float z, float maxDistance)
{
    std::optional<NearbyInteractable> best;
    for (auto [entity, tile] : scene.registry().view<TerrainTile>().each()) {
        if (!tile.data)
            continue;
        for (const map::ZoneObject& o : tile.data->objects) {
            if (o.type != map::ObjectType::Interactable)
                continue;
            const float d = map::footprint_distance(*o.shape, o.pos, o.yaw, x, z);
            if (d <= maxDistance && (!best || d < best->distance))
                best = NearbyInteractable{&o, d};
        }
    }
    return best;
}

void TerrainStreamer::start(const std::optional<std::filesystem::path>& projectRoot)
{
    maps_.clear();
    prefabs_ = {projectRoot.value_or(std::filesystem::path()), {}};
    areaMap_ = 0;
    area_ = -1;
    warnedMap_ = -1;
    if (!projectRoot)
        return;
    const std::filesystem::path root = *projectRoot / "maps";
    for (const std::filesystem::path& file : listFiles(root)) {
        const std::filesystem::path folder = file.parent_path();
        if (file.filename() != "map.json" || folder.parent_path().lexically_normal() != root.lexically_normal() ||
            folder.filename().string().starts_with('.'))
            continue;
        bool exists = false;
        std::string err;
        const std::optional<Json> manifest = readJson(file, exists);
        std::optional<map::MapInfo> info = manifest ? map::parse_manifest(*manifest, err) : std::nullopt;
        if (!info) {
            log::error(std::format("Terrain: {}: {}", file.string(), manifest ? err : "not valid JSON"));
            continue;
        }
        if (const std::optional<Json> areas = readJson(folder / "areas.json", exists)) {
            if (std::optional<map::Areas> parsed = map::parse_areas(*areas, err))
                info->areas = std::move(*parsed);
            else
                log::error(std::format("Terrain: {}: {}", (folder / "areas.json").string(), err));
        } else if (exists) {
            log::error(std::format("Terrain: {}: not valid JSON", (folder / "areas.json").string()));
        }
        if (maps_.contains(info->id)) {
            log::error(std::format("Terrain: {}: map id {} is already used", file.string(), info->id));
            continue;
        }
        const uint16_t id = info->id;
        maps_[id] = {std::make_shared<const map::MapInfo>(std::move(*info)), folder};
    }
}

const map::MapInfo* TerrainStreamer::map(uint16_t id) const
{
    const auto it = maps_.find(id);
    return it == maps_.end() ? nullptr : it->second.info.get();
}

const std::filesystem::path* TerrainStreamer::folder(uint16_t id) const
{
    const auto it = maps_.find(id);
    return it == maps_.end() ? nullptr : &it->second.folder;
}

std::shared_ptr<const TerrainTileData> TerrainStreamer::loadTile(uint16_t id, map::TileKey tile, bool lightmap) const
{
    const auto it = maps_.find(id);
    return it == maps_.end() ? nullptr : load(it->second, tile, lightmap);
}

// ponytail: sync load on the main thread; move to a worker thread if tile crossings hitch.
std::shared_ptr<const TerrainTileData> TerrainStreamer::load(const LoadedMap& map, map::TileKey tile, bool lightmap) const
{
    auto data = std::make_shared<TerrainTileData>();
    data->map = map.info;
    data->tile = tile;
    const std::string stem = std::format("{}_{}", tile.x, tile.z);
    for (const char* ext : {".r16", ".png"}) {
        const std::filesystem::path file = map.folder / "heights" / (stem + ext);
        std::string bytes;
        if (!readFile(file, bytes))
            continue;
        std::string err;
        if (std::optional<map::Heights> heights = map::decode_heights(bytes, ext, err)) {
            data->heights = std::move(*heights);
            break;
        }
        log::error(std::format("Terrain: {}: {}; the tile is flat", file.string(), err));
        break;
    }
    if (data->heights.empty())
        data->heights = map::flat_heights(); // no heights file: flat at heightMin

    const std::filesystem::path objects = map.folder / "objects" / (stem + ".json");
    bool exists = false;
    if (const std::optional<Json> json = readJson(objects, exists)) {
        map::ParsedObjects parsed = map::parse_objects(*json, tile);
        for (const std::string& err : parsed.errors)
            log::error(std::format("Terrain: {}: {}; that object is skipped", objects.string(), err));
        data->objects = std::move(parsed.objects);
    } else if (exists) {
        log::error(std::format("Terrain: {}: not valid JSON; the tile has no objects", objects.string()));
    }
    if (lightmap) { // a missing or bad one: the renderer warns with lightmapError and lights the tile as unbaked
        const std::filesystem::path dir = map.folder / "lightmaps";
        std::string index, irradiance, direction, error;
        TerrainLightmaps parsed;
        if (!readFile(dir / "lightmaps.json", index))
            error = "not baked (no lightmaps/lightmaps.json in its map folder)";
        else if (!parseTerrainLightmaps(index, parsed, error))
            ;
        else if (!readFile(dir / terrainLightmapFile(tile.x, tile.z, false), irradiance) ||
                 !readFile(dir / terrainLightmapFile(tile.x, tile.z, true), direction))
            error = "cannot read its lightmap files";
        else if (checkTerrainLightmap(parsed, tile.x, tile.z, irradiance, direction, error)) {
            data->lightmap = std::move(irradiance);
            data->lightmapDirection = std::move(direction);
        }
        data->lightmapError = error;
    }
    return data;
}

// ponytail: one entity tree per object, created on the main thread with the tile (the Renderer instances repeated
// meshes); a worker load if dense tiles hitch.
void TerrainStreamer::spawnObjects(Scene& scene, entt::entity tile, const LoadedMap& map, const TerrainTileData& data)
{
    const bx::Vec3 origin{float(data.tile.x) * map::kTileSize, 0.0f, float(data.tile.z) * map::kTileSize};
    const std::filesystem::path file = map.folder / "objects" / std::format("{}_{}.json", data.tile.x, data.tile.z);
    for (const map::ZoneObject& object : data.objects) {
        std::string error;
        bool fresh = false;
        if (prefabs_.get(object.prefab, error, &fresh) == nullptr) {
            if (fresh) // once per prefab and session, not once per blade of grass
                log::error(std::format("Terrain: {}: prefab \"{}\": {}; its objects are skipped", file.string(),
                                       object.prefab, error));
            continue;
        }
        if (spawnZoneObject(scene, tile, origin, object, prefabs_, error) == entt::null)
            log::error(std::format("Terrain: {}: {}; that object is skipped", file.string(), error));
    }
}

void TerrainStreamer::update(Scene& scene, EventBus& bus)
{
    entt::registry& registry = scene.registry();
    auto terrains = registry.view<Terrain>();
    const entt::entity owner = terrains.empty() ? entt::null : terrains.front();
    const Terrain* terrain = owner == entt::null ? nullptr : &registry.get<Terrain>(owner);
    const auto loaded = terrain ? maps_.find(static_cast<uint16_t>(terrain->map)) : maps_.end();
    if (terrain && loaded == maps_.end() && warnedMap_ != terrain->map) {
        warnedMap_ = terrain->map;
        log::warn(std::format("Terrain: no map with id {} under maps/", terrain->map));
    }

    std::optional<bx::Vec3> centre;
    if (auto players = registry.view<NetworkLocalPlayer>(); !players.empty())
        centre = worldPosition(scene, players.front());
    else if (const entt::entity camera = activeCamera(scene); camera != entt::null)
        centre = worldPosition(scene, camera);
    const bx::Vec3 at = centre.value_or(bx::Vec3{0.0f, 0.0f, 0.0f});

    std::vector<map::TileKey> wanted;
    if (loaded != maps_.end())
        wanted = tiles_around(*loaded->second.info, {at.x, at.z}, terrain->radius);
    const auto isWanted = [&](const TerrainTile& t) {
        return loaded != maps_.end() && t.map == terrain->map &&
               std::ranges::find(wanted, map::TileKey{t.tx, t.tz}) != wanted.end();
    };

    std::set<std::pair<int32_t, int32_t>> have;
    std::vector<entt::entity> gone;
    for (auto [entity, tile] : registry.view<TerrainTile>().each()) {
        if (isWanted(tile) && have.insert({tile.tx, tile.tz}).second)
            continue;
        gone.push_back(entity);
    }
    for (entt::entity entity : gone)
        scene.destroy(entity);
    for (const map::TileKey key : wanted) {
        if (have.contains({key.x, key.z}))
            continue;
        const entt::entity entity = scene.create(std::format("Terrain Tile {}_{}", key.x, key.z), owner);
        if (entity == entt::null)
            continue;
        registry.get<Transform>(entity).position = {float(key.x) * map::kTileSize, 0.0f, float(key.z) * map::kTileSize};
        const std::shared_ptr<const TerrainTileData> data = load(loaded->second, key, terrain->lightmap);
        registry.emplace<TerrainTile>(entity, terrain->map, key.x, key.z, data);
        spawnObjects(scene, entity, loaded->second, *data);
    }

    if (loaded == maps_.end())
        setArea(bus, 0, -1);
    else
        setArea(bus, loaded->first, map::area_at(loaded->second.info->areas, at.x, at.y, at.z));
}

void TerrainStreamer::setArea(EventBus& bus, uint16_t mapId, int16_t area)
{
    if (mapId == areaMap_ && area == area_)
        return;
    const auto publish = [&](const char* name, uint16_t id, int16_t index) {
        const auto it = maps_.find(id);
        if (index < 0 || it == maps_.end())
            return;
        const map::Area& a = it->second.info->areas.list[static_cast<size_t>(index)];
        bus.publish(name, {{"map", id}, {"id", a.id}, {"name", a.name}}, "kast.terrain");
    };
    publish("area.left", areaMap_, area_);
    areaMap_ = mapId;
    area_ = area;
    publish("area.entered", areaMap_, area_);
}

} // namespace kast
