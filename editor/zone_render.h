// The open zone in the Scene view (edit mode): every declared tile of the ZoneDoc as a TerrainTile and every zone object
// as its prefab, in a scene of their own that SceneViewport draws with the edited scene (Renderer::render's extra), plus
// the tile-border and map-spawn overlays. Reads the ZoneDoc's in-memory state each frame, so edits show at once.
#pragma once

#include <zone/zone_doc.h>

#include <renderer.h>
#include <scene.h>
#include <terrain.h>

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <optional>

class ZoneRender {
public:
    // Mirrors zone (null: clears everything) into scene(); a tile or object file is rebuilt only when it changed (its
    // heights buffer, the manifest, its objects JSON). The generated-terrain material previews through renderer
    // (Renderer::setPreview) before it is saved. Call once a frame before the Scene view renders.
    void sync(const zone::ZoneDoc* zone, const std::filesystem::path& projectRoot, kast::Renderer& renderer);
    // The zone's entities for Renderer::render; null with no zone.
    const kast::Scene* scene() const { return info_ ? &scene_ : nullptr; }
    // Tile borders and the map spawn (with its heading) over the Scene view image at (x, y), width x height (ImGui
    // points), seen through viewProj.
    void drawOverlays(ImDrawList* draw, const float* viewProj, float x, float y, float width, float height) const;

private:
    void syncTiles(const zone::ZoneDoc& zone);
    void syncPreview(const zone::ZoneDoc& zone, kast::Renderer& renderer);
    void syncObjects(const zone::ZoneDoc& zone, const std::filesystem::path& projectRoot);
    void clear(kast::Renderer& renderer);

    struct Tile {
        entt::entity entity = entt::null;
        zone::HeightsPtr heights; // what its data was built from
        bool built = false;
    };
    struct ObjectTile {
        std::vector<entt::entity> entities;
        zone::Json built; // the objects file they were made from
    };

    kast::Scene scene_;
    const zone::ZoneDoc* zone_ = nullptr; // the last synced, for the overlays
    zone::Json manifest_;                 // what tiles_ were built from
    std::shared_ptr<const kast::map::MapInfo> info_;
    zone::TileMap<Tile> tiles_;
    zone::TileMap<ObjectTile> objects_;
    kast::PrefabCache prefabs_;     // for the open project
    zone::Json previewManifest_;    // what the preview was baked from
    zone::TileMap<zone::HeightsPtr> previewHeights_;
    bool previewSet_ = false;
};
