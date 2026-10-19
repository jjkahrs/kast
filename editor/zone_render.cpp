#include "zone_render.h"

#include <zone/terrain_edit.h>

#include <math3d.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

namespace km = kast::map;
constexpr ImU32 kTileLine = IM_COL32(255, 170, 0, 140); // the palette's accent, see-through
constexpr ImU32 kSpawnColor = IM_COL32(255, 204, 0, 255); // its highlight
constexpr float kBorderStep = 4.0f; // metres between the points of a tile border line
constexpr float kLift = 0.2f;       // overlay lines float this far over the ground so it does not hide them

} // namespace

void ZoneRender::clear(kast::Renderer& renderer)
{
    scene_ = {};
    tiles_.clear();
    objects_.clear();
    info_ = nullptr;
    manifest_ = nullptr;
    previewManifest_ = nullptr;
    previewHeights_.clear();
    if (std::exchange(previewSet_, false))
        renderer.setPreview({}, {}, {}, {});
}

void ZoneRender::sync(const zone::ZoneDoc* zone, const std::filesystem::path& projectRoot, kast::Renderer& renderer)
{
    if (zone != zone_ && zone_) // another (or no) document: start over, so nothing built from the old one stays
        clear(renderer);
    zone_ = zone;
    if (!zone)
        return;
    syncTiles(*zone);
    syncPreview(*zone, renderer);
    syncObjects(*zone, projectRoot);
}

// Adapted from the zone builder's view: a tile's data changes (and the renderer rebuilds only that tile's mesh) only
// when its heights or the manifest do.
void ZoneRender::syncTiles(const zone::ZoneDoc& zone)
{
    if (zone.state.manifest != manifest_) {
        manifest_ = zone.state.manifest;
        std::string err;
        auto info = zone.info(err); // validate() reports err
        info_ = info ? std::make_shared<const km::MapInfo>(std::move(*info)) : nullptr;
        for (auto& [key, tile] : tiles_)
            tile.built = false; // every tile's data changes with the manifest
    }
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (info_ && info_->has_tile(it->first)) {
            ++it;
            continue;
        }
        scene_.registry().destroy(it->second.entity);
        it = tiles_.erase(it);
    }
    if (!info_)
        return;
    for (const km::TileRect& r : info_->tiles)
        for (int32_t z = r.z0; z <= r.z1; ++z)
            for (int32_t x = r.x0; x <= r.x1; ++x) {
                const zone::TileKey key{x, z};
                const auto found = zone.state.heights.find(key);
                const zone::HeightsPtr heights = found == zone.state.heights.end() ? nullptr : found->second;
                Tile& tile = tiles_[key];
                if (tile.built && tile.heights == heights)
                    continue;
                if (tile.entity == entt::null) {
                    tile.entity = scene_.registry().create();
                    scene_.registry().emplace<kast::TerrainTile>(tile.entity, int(info_->id), x, z);
                }
                auto data = std::make_shared<kast::TerrainTileData>();
                data->map = info_;
                data->tile = key;
                data->heights = heights ? *heights : km::flat_heights();
                scene_.registry().get<kast::TerrainTile>(tile.entity).data = std::move(data);
                tile.heights = heights;
                tile.built = true;
            }
}

// When the map uses its generated terrain material: re-bakes its texture into the renderer's preview after the heights
// or the manifest change (not mid-stroke), so the view shows it before it is saved.
void ZoneRender::syncPreview(const zone::ZoneDoc& zone, kast::Renderer& renderer)
{
    if (zone.editing() || (zone.state.manifest == previewManifest_ && zone.state.heights == previewHeights_))
        return;
    previewManifest_ = zone.state.manifest;
    previewHeights_ = zone.state.heights;
    std::string err;
    const std::string material = zone::terrain_material_path(zone.folder), texture = zone::terrain_texture_path(zone.folder);
    const auto gradient = zone::parse_gradient(zone.state.manifest, err);
    if (!info_ || info_->material != material || !gradient) {
        if (std::exchange(previewSet_, false))
            renderer.setPreview({}, {}, {}, {});
        return;
    }
    const zone::TerrainImage baked = zone::bake_terrain(*info_, zone.state, *gradient);
    kast::MipImage image{uint16_t(baked.width), uint16_t(baked.height), 1, {}};
    image.rgba.reserve(baked.rgb.size() / 3 * 4);
    for (size_t i = 0; i < baked.rgb.size(); i += 3)
        image.rgba.insert(image.rgba.end(), {baked.rgb[i], baked.rgb[i + 1], baked.rgb[i + 2], 255});
    kast::buildMips(image, true);
    renderer.setPreview(material, zone::terrain_material(texture), texture, std::move(image));
    previewSet_ = true;
}

// Each tile's objects file as entities (kast::spawnZoneObject: the prefab, and the blocking shapes' Colliders, unused
// here); a tile's are rebuilt only when its file's JSON changes.
// ponytail: a changed tile rebuilds all its objects (a drag or a scatter dab: every frame); per-object diffs if dense
// tiles make editing hitch.
void ZoneRender::syncObjects(const zone::ZoneDoc& zone, const std::filesystem::path& projectRoot)
{
    if (projectRoot != prefabs_.projectRoot) { // another project: its prefabs
        prefabs_ = {projectRoot, {}};
        for (auto& [key, tile] : objects_)
            tile.built = nullptr;
    }
    for (auto it = objects_.begin(); it != objects_.end();) {
        const auto file = zone.state.objects.find(it->first);
        if (file != zone.state.objects.end() && file->second == it->second.built) {
            ++it;
            continue;
        }
        for (const entt::entity e : it->second.entities)
            scene_.destroy(e);
        it = objects_.erase(it);
    }
    for (const auto& [key, json] : zone.state.objects) {
        if (objects_.contains(key))
            continue;
        ObjectTile& tile = objects_[key];
        tile.built = json;
        std::string error; // a prefab that does not load: validate() reports it
        for (const km::ZoneObject& object : km::parse_objects(nlohmann::json(json), key).objects)
            if (const entt::entity e = kast::spawnZoneObject(scene_, entt::null, {0.0f, 0.0f, 0.0f}, object, prefabs_, error);
                e != entt::null)
                tile.entities.push_back(e);
    }
}

std::optional<ImVec2> GroundDraw::project(zone::Vec3 p) const
{
    float clip[4];
    const float v[4] = {p[0], p[1], p[2], 1.0f};
    bx::vec4MulMtx(clip, v, viewProj);
    if (clip[3] <= 1e-4f) // behind the camera
        return std::nullopt;
    return ImVec2(x + (clip[0] / clip[3] * 0.5f + 0.5f) * width, y + (0.5f - clip[1] / clip[3] * 0.5f) * height);
}

zone::Vec3 GroundDraw::ground(float gx, float gz) const
{
    // A point on the map's far edge belongs to the tile beyond it: the nearest point on the map.
    const auto c = info.clamp(gx, gz);
    return zone::Vec3{gx, zone::ground_y(info, state, c[0], c[1]).value_or(0.0f) + kLift, gz};
}

void GroundDraw::line(std::array<float, 2> a, std::array<float, 2> b, ImU32 color, float thickness) const
{
    const int steps = std::max(1, int(std::hypot(b[0] - a[0], b[1] - a[1]) / kBorderStep));
    std::optional<ImVec2> last;
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const auto p = project(ground(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t));
        if (p && last)
            draw->AddLine(*last, *p, color, thickness);
        last = p;
    }
}

void GroundDraw::circle(float cx, float cz, float radius, ImU32 color, float thickness) const
{
    constexpr int kSegments = 64;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = 2.0f * bx::kPi * float(i) / kSegments, a1 = 2.0f * bx::kPi * float(i + 1) / kSegments;
        line({cx + radius * std::cos(a0), cz + radius * std::sin(a0)},
             {cx + radius * std::cos(a1), cz + radius * std::sin(a1)}, color, thickness);
    }
}

void ZoneRender::drawOverlays(ImDrawList* draw, const float* viewProj, float x, float y, float width, float height) const
{
    if (!zone_ || !info_)
        return;
    const GroundDraw g{draw, viewProj, x, y, width, height, *info_, zone_->state};
    draw->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
    for (const auto& [key, tile] : tiles_) {
        const float x0 = key.x * km::kTileSize, z0 = key.z * km::kTileSize, x1 = x0 + km::kTileSize, z1 = z0 + km::kTileSize;
        g.line({x0, z0}, {x1, z0}, kTileLine, 1.5f);
        g.line({x0, z0}, {x0, z1}, kTileLine, 1.5f);
        if (!info_->has_tile({key.x, key.z + 1}))
            g.line({x0, z1}, {x1, z1}, kTileLine, 1.5f);
        if (!info_->has_tile({key.x + 1, key.z}))
            g.line({x1, z0}, {x1, z1}, kTileLine, 1.5f);
    }
    const zone::Vec3 spawn = g.ground(info_->spawn[0], info_->spawn[2]);
    if (const auto p = g.project(spawn)) {
        draw->AddCircleFilled(*p, 6.0f, kSpawnColor);
        const float h = info_->spawnHeading;
        if (const auto q = g.project(g.ground(spawn[0] + 6.0f * std::sin(h), spawn[2] + 6.0f * std::cos(h))))
            draw->AddLine(*p, *q, kSpawnColor, 3.0f);
        draw->AddText(ImVec2(p->x + 8.0f, p->y - 8.0f), kSpawnColor, "spawn");
    }
    draw->PopClipRect();
}
