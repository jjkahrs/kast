// The Map tab's Tiles section (docs/editor/map-builder.html#tiles): the open map (MapSession) top-down in one grid, a
// cell per tile (+z up; fitted whole, zoomed with the wheel, panned by a drag), each showing its tile scene
// (tile_scenes.h): the cell's state color, the entity count, a terrain thumbnail from the map's heights and
// terrainColors, and the map's spawn and portal areas over them. What a click does is the grid's mode:
// - Scenes: the bounds at least 8 x 8 cells. Clicking a declared cell opens its scene additively, closes it (Remove
//   Scene, with its unsaved-changes prompt) or, with none, creates an empty one that Ctrl+S writes to
//   maps/<map>/scenes/<tx>_<tz>.scene.
// - Tiles: the bounds and a free cell around them. Clicking a free cell (+) adds that tile, a tile removes it (the
//   caller does both: draw returns the cell).
// Orphan scenes (on undeclared tiles), root entities outside their tile and bad file names in scenes/ are warned about
// once each (the section's list and the Console); they never block a save. Clicks do nothing during Play and in prefab
// edit mode.
#pragma once

#include "document.h"
#include "map_session.h"
#include "tile_grid.h"

#include <tile_scenes.h>

#include <bgfx/bgfx.h>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class MapTiles {
public:
    enum class Mode { Scenes, Tiles };
    static constexpr int kMinGrid = 8; // Scenes: cells per side at least, so a small map shows the cells around it

    // Once a frame, also while the section is collapsed or the tab behind: the map's scene files, entity counts and
    // warnings follow the map and the open scenes (read again every half second); new warnings go to the Console.
    void update(const Document& doc, const MapSession& session);
    // The section's body: the mode switch, the grid, its legend and the warnings. Returns the cell clicked in Tiles
    // mode (nothing while locked); a click in Scenes mode is click()'s.
    std::optional<kast::map::TileKey> draw(Document& doc, const MapSession& session, FileActions& files,
                                           const kast::map::MapInfo& info, bool locked);
    // What clicking cell t in Scenes mode does (--map-click too): opens, closes or creates its tile scene; nothing on an
    // undeclared tile, without a map or while read-only. Logs "Map: ..." lines.
    void click(Document& doc, const MapSession& session, FileActions& files, kast::map::TileKey t);
    // Frees the thumbnails' textures (before bgfx shuts down).
    void shutdown();
    // --grid-view: centres the grid on tile t at zoom times the fitted cells and logs the view: Scenes "Map: view ..."
    // once its thumbnails are built, Tiles "Map: tile grid view ...".
    void look(kast::map::TileKey t, float zoom)
    {
        grid_.look(t, zoom);
        logView_ = looked_ = true;
    }
    Mode mode() const { return mode_; }
    // --grid-mode: what clicks do from now on.
    void setMode(Mode mode) { mode_ = mode; }
    // "scenes", "tiles": the Mode of that name; nullopt if none.
    static std::optional<Mode> modeNamed(std::string_view name);

private:
    // A tile scene file as last read (its modification time, so it is read again only when it changes).
    struct FileSummary {
        std::filesystem::file_time_type mtime;
        std::optional<kast::TileSceneSummary> summary;
        std::string error;
    };
    struct Thumbnail {
        mapedit::HeightsPtr heights; // the MapState heights buffer it was made from (null: flat), kept so it is not reused
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    };

    void refresh(const Document& doc, const mapedit::MapDoc& z, const kast::map::MapInfo& info);
    // The absolute file of tile t's scene.
    std::filesystem::path tileFile(const std::filesystem::path& root, const std::string& folder, kast::map::TileKey t) const;
    // The visible declared cells' thumbnails built (a few a frame), the rest freed.
    void syncThumbnails(const mapedit::MapDoc& z, const kast::map::MapInfo& info);
    std::optional<kast::map::TileKey> drawGrid(Document& doc, const MapSession& session, FileActions& files,
                                               const kast::map::MapInfo& info, bool locked);
    void drawWarnings() const;

    Mode mode_ = Mode::Scenes;
    std::string folder_; // the map the state below is for
    uint32_t generation_ = UINT32_MAX;
    double nextRefresh_ = 0.0;
    std::set<kast::map::TileKey, mapedit::TileLess> sceneFiles_; // tiles with a file in scenes/
    std::map<kast::map::TileKey, size_t, mapedit::TileLess> entities_; // per tile scene, open or on disk
    std::map<std::string, FileSummary> files_;
    std::vector<std::string> warnings_;
    std::set<std::string> logged_; // the warnings shown in the Console while they last
    std::map<kast::map::TileKey, Thumbnail, mapedit::TileLess> thumbnails_;
    std::string thumbnailColors_; // the terrainColors (dumped) the thumbnails were made with
    size_t thumbnailsPending_ = 0; // visible ones not built yet (a few a frame)
    TileGrid grid_;
    bool logView_ = false; // Scenes: a new map or --grid-view: "Map: view ..." once its thumbnails are built
    bool looked_ = false;  // Tiles: --grid-view: scroll to the grid, "Map: tile grid view ..." once drawn
};
