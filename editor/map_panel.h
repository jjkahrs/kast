// The Map tab (docs/editor/map-builder.html): the open map (MapSession) in one window of collapsing sections. A top
// bar (the map, Save Map, Undo/Redo, the unsaved marker, the Scene view tool buttons), then Map (pick or create the map
// the scene's Terrain entity names, its manifest), Tiles (map_tiles.h: the one tile grid, adding and removing tiles and
// opening tile scenes), Terrain (map_terrain.cpp: brushes, height range, heightmap PNGs, Generate, Height colors, the
// terrain material), Areas, Spawns (+ patrol paths), Objects (+ the Scatter brush) and Bake (map_objects.cpp), and
// Zones (map_zones.cpp: the map's zones, painted on the Tiles grid, their zone scenes) and Problems (the last validation). Read-only during Play and in prefab edit mode. Unsaved-changes prompt before the map
// changes (here or in the Inspector); scene-wide prompts and Ctrl+S go through FileActions::extra (main.cpp). The active tool gets the
// Scene view's left mouse through sceneInput (SceneViewport::input) and draws over it with drawSceneOverlay; with no
// tool, while the tab is in front, the map's handles (area vertices, spawns, path points, objects) drag there.
#pragma once

#include "document.h"
#include "map_render.h" // GroundDraw
#include "map_session.h"
#include "map_tiles.h"
#include "viewport.h"

#include <mapedit/process_runner.h>
#include <mapedit/terrain_edit.h>
#include <mapedit/map_doc.h>
#include <mapedit/map_edit.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>

class MapPanel {
public:
    static constexpr const char* kWindow = "Map";
    // Scene view tools: while one is active, left mouse in the Scene view goes to it instead of entity picking and the
    // gizmo (sceneInput). Esc clears it.
    enum class Tool {
        None, Raise, Lower, Flatten, AreaRect, AreaPolygon, PlaceSpawn, PathPoints, SetSpawn, PlaceObject, Scatter
    };

    // settingsFile: the editor settings (editor_settings.h), where Bake's kast_mapbake path is kept ("mapbake").
    explicit MapPanel(std::filesystem::path settingsFile);

    // The window and its modals; call once a frame after session.update.
    void draw(Document& doc, MapSession& session, FileActions& files);
    // Saves the map (MapSession::save); a blocked save opens Problems and brings the tab forward. Open zone scenes of
    // renamed zones follow their files.
    bool save(Document& doc, MapSession& session);
    // --map-section: expand these sections ("Map", "Tiles", "Zones", "Terrain", "Problems") and collapse the others, each the next time
    // it is drawn.
    void showSections(const std::vector<std::string>& names) { showSections_ = std::set(names.begin(), names.end()); }
    Tool tool() const { return tool_; }
    // --map-tool: makes tool active, as its button does (ignored while there is no map or the tab is read-only).
    void setTool(Tool tool) { tool_ = tool; }
    // "Raise", "SetSpawn", ...: the Tool of that name; nullopt if none.
    static std::optional<Tool> toolNamed(std::string_view name);
    // --map-select: selects the area or spawn with this id, the map object "object:tx,tz,index" or the zone "zone:id".
    void select(const MapSession& session, const std::string& id);
    // Bake Navmesh: kast_mapbake on the saved, valid map (else logs why not); its output and exit code go to the
    // Console. False if it did not start.
    bool startBake(MapSession& session);
    bool baking() const { return bake_.running(); }
    std::optional<int> bakeExit() const { return bakeExit_; }
    // --mapbake: the kast_mapbake.exe for this run (not saved to the settings).
    void setMapbake(std::string exe) { mapbake_ = std::move(exe); }
    // Moves kast_mapbake's output to the Console and logs its exit code once it ends; draw calls it every frame.
    void pollBake();
    // --prompt-remove-tile: opens the remove-tile confirm dialog for t, as clicking it in the tile grid does.
    void askRemoveTile(kast::map::TileKey t) { removeTile_ = t; }
    // --grid-view, --grid-mode, --map-click and shutdown: the Tiles section's grid (MapTiles).
    MapTiles& tiles() { return tiles_; }
    // Zone edits as the Zones section makes them, one undo step each, logged ("Map: ..."); the zone must exist.
    // --rename-zone and --delete-zone too.
    static void renameZone(mapedit::MapDoc& z, const kast::map::MapInfo& info, const std::string& from, const std::string& to);
    static void deleteZone(mapedit::MapDoc& z, const kast::map::MapInfo& info, const std::string& id);
    // Points the first Terrain entity (added if there is none) at map id; marks the scene dirty.
    static void setTerrainMap(Document& doc, int id);

    // Before session.update each frame: the first Terrain's map changed elsewhere (the Inspector) while the map has
    // unsaved changes. Puts the old id back and asks first (the unsaved-map prompt); the new id is applied after Save
    // or Don't Save, Cancel keeps the old map.
    void guardMapChange(Document& doc, const MapSession& session);
    // SceneViewport::input: the active tool's Scene view mouse (the ground under it, clicks, strokes). True while a
    // tool is active: the left mouse is the map's. With no tool, a drag from a map handle (Map tab in front) claims
    // it too.
    bool sceneInput(MapSession& session, const SceneViewport::Input& input);
    // SceneViewport::overlay, after MapRender's: areas, spawns, paths and objects, and the active tool's overlay (the
    // brush ring, the area being drawn), on the tiles the Scene view meshes around centre (the camera's pivot).
    void drawSceneOverlay(const MapSession& session, ImDrawList* draw, const float* viewProj, float x, float y, float width,
                          float height, std::array<float, 2> centre) const;

private:
    // A map handle being dragged in the Scene view (no tool).
    struct Drag {
        enum class Kind { None, AreaVertex, Spawn, PathPoint, Object } kind = Kind::None;
        size_t index = 0; // the vertex or path point
    };
    // A map change waiting on the unsaved-map prompt.
    struct Pending {
        enum class Kind { None, PickMap, NewMap } kind = Kind::None;
        int map = 0;
    };

    bool header(const char* label, const char* section, bool defaultOpen = false);
    void drawTopBar(Document& doc, MapSession& session);
    void drawTerrainEntity(Document& doc, const MapSession& session);
    void drawMap(Document& doc, MapSession& session);
    void drawManifest(mapedit::MapDoc& z, const MapSession& session);
    // An instance map's fields (map.json maxPlayers, emptyTimeoutSec, exit), from the JSON: editable while invalid.
    void drawInstance(mapedit::MapDoc& z, const MapSession& session);
    // The Tiles section: MapTiles' grid; a click in its Tiles mode adds a free cell's tile or asks to remove a tile.
    void drawTiles(Document& doc, MapSession& session, FileActions& files, bool locked);
    void drawProblems(const MapSession& session);
    // map_zones.cpp
    void drawZones(Document& doc, MapSession& session, FileActions& files);
    void drawZoneFields(Document& doc, MapSession& session, FileActions& files, const kast::map::MapInfo& info,
                        const kast::map::Zone& zone);
    // A paint stroke (MapTiles' Zones mode): tile t into the selected zone; the stroke is one undo step, ended (and
    // logged) by endZoneStroke once the mouse is up.
    void paintZone(mapedit::MapDoc& z, const kast::map::MapInfo& info, kast::map::TileKey t);
    void endZoneStroke(MapSession& session);
    void drawUnsavedPrompt(Document& doc, MapSession& session);
    void drawNewMapDialog(Document& doc, MapSession& session);
    void drawRemoveTilePrompt(MapSession& session);
    void toolButton(const char* label, Tool tool);
    // map_terrain.cpp
    void drawTerrain(MapSession& session);
    void drawGenerate(mapedit::MapDoc& z, const kast::map::MapInfo& info);
    void drawTerrainColors(mapedit::MapDoc& z);
    void drawHeightRangePrompt(MapSession& session);
    void setHeightRange(mapedit::MapDoc& z, float min, float max);
    // A sculpt brush's Scene view mouse: Shift+click (Flatten) picks the height; a left drag is one undo step.
    void sculptInput(mapedit::MapDoc& z, const kast::map::MapInfo& info, bool click);
    // Ends a sculpt or scatter stroke or a handle drag (its undo step) once the mouse is up, the tool changed or the
    // map went away.
    void endStroke(MapSession& session);
    // map_objects.cpp
    void drawAreas(MapSession& session);
    void drawSpawns(MapSession& session);
    void drawObjects(MapSession& session);
    void drawObjectFields(MapSession& session);
    void drawScatter(const MapSession& session);
    void drawBake(MapSession& session);
    // PlaceObject's prefab and the Scatter brush's first one, once the project has prefabs.
    void defaultPrefabs(const MapSession& session);
    // Drops selections that no longer exist (an undo, another map).
    void checkSelection(const mapedit::MapDoc* z);
    // The area, spawn, path and object tools' Scene view mouse, and handle drags with no tool. True: the mouse is the
    // map's this frame.
    bool objectsInput(const MapSession& session, mapedit::MapDoc& z, const kast::map::MapInfo& info, const GroundDraw& g,
                      bool click);
    // A Scatter stroke: a dab on press, another each time the brush moves its radius; one undo step (endStroke).
    void scatterInput(mapedit::MapDoc& z, const kast::map::MapInfo& info, bool click);
    // With no tool: a handle under the mouse starts a drag (true); else the area under it is selected (false).
    bool pickHandle(mapedit::MapDoc& z, const GroundDraw& g);
    void drawObjectOverlays(const GroundDraw& g) const;
    // The heightmap file dialogs' answers (SDL may call back on another thread).
    static void onPngChosen(void* userdata, const char* const* files, int filter);
    void pollPngDialog(MapSession& session);
    // Runs pending now, or after the prompt if the map has unsaved changes.
    void request(Document& doc, MapSession& session, Pending pending);
    void run(Document& doc, MapSession& session, const Pending& pending);

    std::optional<std::set<std::string>> showSections_;
    std::set<std::string> sectionsShown_; // the sections showSections_ has been applied to
    Tool tool_ = Tool::None;
    Pending pending_;
    bool askUnsaved_ = false;
    bool openNewMap_ = false;
    std::string newFolder_, newName_, newError_;
    int newId_ = 0, newWidth_ = 2, newDepth_ = 2;
    std::optional<kast::map::TileKey> removeTile_; // asking to confirm its removal
    MapTiles tiles_;
    bool tabFront_ = false; // the Map tab showed this frame: its handles are live
    size_t shownProblems_ = 0;
    bool openProblems_ = false;  // new problems: expand the section
    bool focusProblems_ = false; // a blocked save: bring the tab forward until it shows

    // Terrain
    mapedit::Brush brush_;
    float strength_ = 4.0f; // Raise/Lower: metres per second at the centre; Flatten: the fraction per second
    bool stroking_ = false;
    std::optional<mapedit::Vec3> hover_; // the ground under the mouse while a tool is active
    mapedit::TileKey tile_;              // heightmap import/export: the last tile clicked
    float rangeMin_ = 0, rangeMax_ = 0;
    size_t rangeClips_ = 0;
    bool askRange_ = false;
    bool rangeEdited_ = false; // Min/Max typed but not applied: keep them
    mapedit::NoiseParams noise_;
    mapedit::Gradient gradient_;    // the stops shown; the manifest's while no edit is in progress
    bool colorsEditing_ = false; // a stop edit's undo step is open
    bool pngDialogOpen_ = false;
    bool pngExport_ = false; // what the open dialog is for
    std::mutex pngMutex_;    // guards the answer:
    bool pngChosen_ = false;
    std::string pngPath_; // UTF-8; empty: cancelled

    // Areas, spawns, objects (map_objects.cpp)
    std::string selectionFolder_;              // the map the selections are for
    std::optional<size_t> area_;               // selected area (index into areas.json "areas")
    std::string spawn_;                        // selected spawn id
    std::optional<mapedit::ObjectRef> object_;    // selected map object
    std::vector<mapedit::Point> polygon_;         // AreaPolygon: the points so far
    std::optional<std::array<mapedit::Point, 2>> rect_; // AreaRect: the drag's start and current corner
    Drag drag_;
    std::string dragLog_; // what the drag moved, logged when it ends
    std::string placePrefab_; // PlaceObject's prefab and type
    kast::map::ObjectType placeType_ = kast::map::ObjectType::Obstacle;
    mapedit::Scatter scatter_; // the brush; its prefabs are ticked in the Objects section
    bool scatterSeeded_ = false; // the first project prefab ticked once
    bool scatterErase_ = false;  // the stroke removes visuals instead
    bool scattering_ = false;    // stroking_ is a scatter stroke
    size_t strokeCount_ = 0;     // visuals placed or removed by it
    int scatterSeed_ = 1;
    mapedit::ScatterRng scatterRng_{1}; // reseeded when the seed changes, so a seed replays the same strokes
    std::optional<mapedit::Point> lastDab_;
    std::string dataText_; // the selected interactable's "data" being typed
    uint32_t dataFor_ = 0; // whose dataText_ it is (interactable id)
    std::string dataError_;

    // Zones (map_zones.cpp)
    std::string zone_;                   // the selected zone's id
    std::string zoneIdFor_, zoneIdText_; // the Id field being typed, for that zone
    std::string flagsText_, flagsError_; // its flags being typed
    bool zoneStroke_ = false;            // a paint stroke's undo step is open
    size_t strokeTiles_ = 0;             // tiles it moved

    // Bake
    std::filesystem::path settingsFile_;
    std::string mapbake_; // the kast_mapbake.exe setting; empty: find_mapbake's default search
    bool mapbakeEdited_ = false; // typed, saved once the field is left
    mapedit::ProcessRunner bake_;
    std::vector<std::string> bakeLog_;
    std::optional<int> bakeExit_;
};
