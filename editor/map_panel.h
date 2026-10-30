// The Map tab (docs/editor/map-builder.html): the open zone (ZoneSession) top-down, one cell per tile of its 8 x 8
// bounds (+z up, as the Zone tab's tile grid), each showing its tile scene (tile_scenes.h): the cell's state color, the
// entity count, a terrain thumbnail from the zone's heights and terrainColors, and the map's spawn and portal areas over
// them. Clicking a declared cell opens its scene additively, closes it (Remove Scene, with its unsaved-changes prompt) or,
// with none, creates an empty one that Ctrl+S writes to maps/<map>/scenes/<tx>_<tz>.scene. Orphan scenes (on undeclared
// tiles), root entities outside their tile and bad file names in scenes/ are warned about once each (its Problems and the
// Console); they never block a save. Read-only during Play and in prefab edit mode.
#pragma once

#include "document.h"
#include "zone_session.h"

#include <tile_scenes.h>

#include <bgfx/bgfx.h>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class MapPanel {
public:
    static constexpr const char* kWindow = "Map";
    static constexpr int kGrid = 8; // cells per side: a map's largest size

    // The window; call once a frame after zone.update.
    void draw(Document& doc, const ZoneSession& zone, FileActions& files);
    // What clicking cell t does (--map-click too): opens, closes or creates its tile scene; nothing on an undeclared
    // tile, without a zone or while read-only. Logs "Map: ..." lines.
    void click(Document& doc, const ZoneSession& zone, FileActions& files, kast::map::TileKey t);
    // Frees the thumbnails' textures (before bgfx shuts down).
    void shutdown();

private:
    // A tile scene file as last read (its modification time, so it is read again only when it changes).
    struct FileSummary {
        std::filesystem::file_time_type mtime;
        std::optional<kast::TileSceneSummary> summary;
        std::string error;
    };
    struct Thumbnail {
        zone::HeightsPtr heights; // the ZoneState heights buffer it was made from (null: flat), kept so it is not reused
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    };

    // The zone's scene files, entity counts and warnings again (every half second, and when the zone or the scenes
    // change): new warnings go to the Console.
    void refresh(const Document& doc, const zone::ZoneDoc& z, const kast::map::MapInfo& info);
    // The absolute file of tile t's scene.
    std::filesystem::path tileFile(const std::filesystem::path& root, const std::string& folder, kast::map::TileKey t) const;
    void syncThumbnails(const zone::ZoneDoc& z, const kast::map::MapInfo& info);
    void drawGrid(Document& doc, const ZoneSession& zone, FileActions& files, const kast::map::MapInfo& info, bool locked);

    std::string folder_; // the zone the state below is for
    uint32_t generation_ = UINT32_MAX;
    double nextRefresh_ = 0.0;
    std::set<kast::map::TileKey, zone::TileLess> sceneFiles_; // tiles with a file in scenes/
    std::map<kast::map::TileKey, size_t, zone::TileLess> entities_; // per tile scene, open or on disk
    std::map<std::string, FileSummary> files_;
    std::vector<std::string> warnings_;
    std::set<std::string> logged_; // the warnings shown in the Console while they last
    std::map<kast::map::TileKey, Thumbnail, zone::TileLess> thumbnails_;
    std::string thumbnailColors_; // the terrainColors (dumped) the thumbnails were made with
};
