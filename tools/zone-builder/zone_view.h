// The zone builder's 3D view: the open map's tiles drawn by kast::Renderer (with the map's material, as in game) and
// kast::PostProcess straight into the backbuffer behind the ImGui windows, the orbit camera, ground picking and the
// overlays (ImGui lines projected from the ground). Zone objects show as their prefabs.
#pragma once
#include "terrain_edit.h"
#include "zone_doc.h"

#include <camera.h>
#include <post_process.h>
#include <renderer.h>
#include <scene.h>
#include <terrain.h>

#include <bgfx/bgfx.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace zone {

class ZoneView {
public:
    // Uses views first .. first + Renderer::kShadowViews + PostProcess::kViews.
    explicit ZoneView(bgfx::ViewId first) : shadowViews_(first), post_(first + kast::Renderer::kShadowViews) {}

    void init();
    void shutdown();
    // Renders doc (none: just the background) at the window's backbuffer size.
    void render(const ZoneDoc* doc, const std::filesystem::path& projectRoot, uint16_t width, uint16_t height);
    // Camera input from the mouse while no ImGui window wants it: right drag or Alt + left drag orbits, middle drag
    // pans, the wheel zooms.
    void updateCamera(float viewportHeight);
    // Frames every declared tile.
    void frameMap(const ZoneDoc& doc);
    // The ground point under window pixel (x, y), if any.
    std::optional<Vec3> pick(const ZoneDoc& doc, float x, float y) const;
    // Window pixel of a world point; nullopt behind the camera.
    std::optional<std::array<float, 2>> project(Vec3 p) const;
    // Tile borders and the map spawn (with its heading).
    void drawOverlays(const ZoneDoc& doc) const;
    // The ground at map (x, z), lifted a little so lines on it are not hidden; y 0 before the first render.
    Vec3 groundPoint(const ZoneDoc& doc, float x, float z) const;
    // A line from a to b (x, z) following the ground, and a circle on it.
    void drawGroundLine(const ZoneDoc& doc, std::array<float, 2> a, std::array<float, 2> b, uint32_t color,
                        float thickness) const;
    void drawGroundCircle(const ZoneDoc& doc, float x, float z, float radius, uint32_t color) const;

    EditorCamera camera;

private:
    // Mirrors the doc's declared tiles as TerrainTile entities; a tile's data changes (and the renderer rebuilds only
    // that tile's mesh) only when its heights or the manifest do.
    void sync(const ZoneDoc& doc);
    // When the map uses its generated terrain material: re-bakes its texture into the renderer's preview after the
    // heights or the manifest change (not mid-stroke), so the view shows it before it is saved.
    void syncPreview(const ZoneDoc& doc);
    // Mirrors each tile's objects file as entities (kast::spawnZoneObject: the prefab, and the blocking shapes'
    // Colliders, unused here); a tile's are rebuilt only when its file's JSON changes.
    void syncObjects(const ZoneDoc& doc, const std::filesystem::path& projectRoot);

    struct Tile {
        entt::entity entity = entt::null;
        HeightsPtr heights; // what data was built from
        bool built = false;
    };

    bgfx::ViewId shadowViews_;
    kast::PostProcess post_;
    kast::Renderer renderer_;
    kast::Scene scene_;
    Json manifest_; // what tiles_ were built from
    std::shared_ptr<const kast::map::MapInfo> info_;
    TileMap<Tile> tiles_;
    struct ObjectTile {
        std::vector<entt::entity> entities;
        Json built; // the objects file they were made from
    };
    TileMap<ObjectTile> objects_;
    kast::PrefabCache prefabs_; // for the open project
    Json previewManifest_;           // what the preview was baked from
    TileMap<HeightsPtr> previewHeights_;
    float viewProj_[16] = {};
    float width_ = 1, height_ = 1;
    enum class Drag { None, Orbit, Pan } drag_ = Drag::None;
};

} // namespace zone
