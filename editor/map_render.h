// The open map in the Scene view (edit mode): the MapDoc's declared tiles near the camera as TerrainTiles and every
// map object as its prefab, in a scene of their own that SceneViewport draws with the edited scene (Renderer::render's
// extra), plus the tile-border and map-spawn overlays. Reads the MapDoc's in-memory state each frame, so edits show at once.
#pragma once

#include <mapedit/map_doc.h>

#include <renderer.h>
#include <scene.h>
#include <terrain.h>

#include <imgui.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

// Draws on a map's ground over the Scene view image at (x, y), width x height (ImGui points), seen through viewProj:
// the map overlays (MapRender) and the Map tab's tool overlays (MapPanel). Lines follow the ground (in-memory
// heights) a little above it, so it does not hide them; only on the meshed tiles around centre (mapedit::in_edit_view).
struct GroundDraw {
    ImDrawList* draw;
    const float* viewProj;
    float x, y, width, height;
    const kast::map::MapInfo& info;
    const mapedit::MapState& state;
    std::array<float, 2> centre; // the camera's pivot, x and z

    // The image point of world point p; nullopt behind the camera or off the meshed tiles.
    std::optional<ImVec2> project(mapedit::Vec3 p) const;
    // The ground at map (gx, gz), lifted; a point past the map's edge takes the nearest point on the map's height.
    mapedit::Vec3 ground(float gx, float gz) const;
    void line(std::array<float, 2> a, std::array<float, 2> b, ImU32 color, float thickness) const;
    void circle(float cx, float cz, float radius, ImU32 color, float thickness = 2.0f) const;
};

class MapRender {
public:
    // Mirrors map (null: clears everything) into scene(): its declared tiles within mapedit::kEditRadius of centre (the
    // camera's pivot, x and z), built as they come near and dropped as they go; a tile or object file is rebuilt only
    // when it changed (its heights buffer, what the manifest gives its mesh, mapedit::same_terrain_mesh; its objects
    // JSON). The generated-terrain material previews through renderer (Renderer::setPreview) before it is saved. Call
    // once a frame before the Scene view renders.
    void sync(const mapedit::MapDoc* doc, std::array<float, 2> centre, const std::filesystem::path& projectRoot,
              kast::Renderer& renderer);
    // The map's entities for Renderer::render; null with no map.
    const kast::Scene* scene() const { return info_ ? &scene_ : nullptr; }
    // The meshed tiles' borders and the map spawn (with its heading) over the Scene view image at (x, y), width x
    // height (ImGui points), seen through viewProj.
    void drawOverlays(ImDrawList* draw, const float* viewProj, float x, float y, float width, float height) const;

private:
    void syncTiles(const mapedit::MapDoc& doc, std::array<float, 2> centre);
    void syncPreview(const mapedit::MapDoc& doc, kast::Renderer& renderer);
    void syncObjects(const mapedit::MapDoc& doc, const std::filesystem::path& projectRoot);
    void clear(kast::Renderer& renderer);

    struct Tile {
        entt::entity entity = entt::null;
        mapedit::HeightsPtr heights; // what its data was built from
        bool built = false;
    };
    struct ObjectTile {
        std::vector<entt::entity> entities;
        mapedit::Json built; // the objects file they were made from
    };

    kast::Scene scene_;
    const mapedit::MapDoc* doc_ = nullptr; // the last synced, for the overlays
    mapedit::Json manifest_;                 // what tiles_ were built from
    std::shared_ptr<const kast::map::MapInfo> info_;
    std::array<float, 2> centre_{}; // what tiles_ are around
    mapedit::TileMap<Tile> tiles_; // the meshed tiles, around the camera
    size_t loggedTiles_ = 0;    // tiles_.size() last logged
    mapedit::TileMap<ObjectTile> objects_;
    kast::PrefabCache prefabs_;     // for the open project
    mapedit::Json previewManifest_;    // what the preview was baked from
    mapedit::TileMap<mapedit::HeightsPtr> previewHeights_;
    bool previewSet_ = false;
};
