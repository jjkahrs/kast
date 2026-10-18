// The zone builder's session (open project and map, the last save's problems) and its ImGui panels: menu bar, Maps,
// Map (manifest), Terrain, Areas, Spawns, Objects, Problems and Log, plus the dialogs; the 3D view's tools
// (zone_objects.cpp: areas, spawns, paths and zone objects).
#pragma once
#include "zone_edit.h"
#include "process_runner.h"
#include "zone_io.h"
#include "zone_view.h"

#include <imgui.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace zone {

// What is open, and the actions the menus and the command line share. No ImGui.
struct Session {
    std::optional<Project> project;
    std::optional<ZoneDoc> doc;
    std::vector<ZoneError> problems; // the last validation or save; empty when it passed
    std::vector<std::string> log;    // newest last
    std::vector<std::string> materials; // the project's .mat files, project-relative
    std::vector<std::string> npcs;      // the project's NPC definition names (npc_names)
    std::vector<std::string> prefabs;   // the project's .prefab files, project-relative
    bool recycle = true;             // save_map: deleted files go to the Recycle Bin

    // Bake Navmesh: kast_mapbake on the open map, its output and exit code; the exe path setting.
    ProcessRunner bake;
    std::vector<std::string> bakeLog;
    std::optional<int> bakeExit;
    std::string mapbakeSetting;           // empty: find_mapbake's default search
    std::filesystem::path settingsFile;   // %APPDATA%/Kast/ZoneBuilder/zone_builder.json; empty: not kept

    bool openProject(const std::filesystem::path& root);
    bool openMap(const std::string& folder);
    bool newMap(const std::string& folder, const std::string& name, uint16_t id, int width, int depth);
    bool save();
    void print(std::string line); // to the Log panel and kast::log
    void scanMaterials(); // materials and prefabs from the project's files

    // Terrain edits, one undo step each (an interactive stroke is one step too, see Panels).
    bool sculpt(const Brush& brush, float x, float z, float amount);
    bool setHeightRange(float min, float max);
    bool importPng(TileKey tile, const std::filesystem::path& file);
    bool exportPng(TileKey tile, const std::filesystem::path& file);
    bool generateHeights(const NoiseParams& params);
    bool setTerrainColors(const Gradient& gradient); // map.json "terrainColors"
    // Assigns the generated terrain material (terrain_material_path) with "terrainUv": "map", adding the default
    // terrainColors if there are none; save writes the texture and material.
    bool createTerrainMaterial();

    // Starts kast_mapbake on the saved, valid open map (else prints why not). pollBake moves its output to bakeLog
    // and prints the exit code once it ends; call every frame.
    bool startBake();
    void pollBake();
    void loadSettings(); // mapbakeSetting from settingsFile
    void saveSettings();
};

class Panels {
public:
    explicit Panels(ZoneView& view) : view_(view) {}
    void draw(Session& session, SDL_Window* window);
    // For screenshots (--panel, --select): bring a panel's tab to the front; select an area or spawn by id, or a zone
    // object as "object:tx,tz,index".
    void showPanel(std::string name) { showPanel_ = std::move(name); }
    void select(const Session& session, const std::string& id);
    void askRemoveTile(TileKey t) { removeTile_ = t; } // the confirm dialog, as clicking a tile in the grid opens it
    // Close requested: true to quit now, false while the unsaved-changes prompt is up.
    bool onQuitRequested(Session& session);

private:
    // Something that would drop unsaved edits; run once they are saved or discarded.
    struct Pending {
        enum class Kind { None, OpenProject, OpenMap, NewMap, Quit } kind = Kind::None;
        std::string arg;
    };
    enum class Tool {
        None, Raise, Lower, Flatten, AreaRect, AreaPolygon, PlaceSpawn, PathPoints, SetSpawn, PlaceObject, Scatter
    };
    // A handle being dragged in the 3D view (Tool::None).
    struct Drag {
        enum class Kind { None, AreaVertex, Spawn, PathPoint, Object } kind = Kind::None;
        size_t index = 0; // the vertex or path point
    };
    // A file dialog's answer, for what asked.
    enum class Dialog { OpenProject, ImportPng, ExportPng };

    void drawMenu(Session& session, SDL_Window* window);
    void drawMaps(Session& session);
    void drawManifest(Session& session);
    void drawProblems(Session& session);
    void drawLog(Session& session);
    void drawUnsavedPrompt(Session& session);
    void drawNewMapDialog(Session& session);
    void shortcuts(Session& session);
    // Runs pending now, or asks first if the open map has unsaved changes.
    void request(Session& session, Pending pending);
    void run(Session& session, const Pending& pending);
    void pollDialogs(Session& session);
    void drawTerrain(Session& session, SDL_Window* window);
    void drawGenerate(Session& session, bool hasRange);
    void drawTerrainColors(Session& session);
    void drawHeightRangePrompt(Session& session);
    void drawTileGrid(Session& session, const kast::map::MapInfo& info);
    void drawRemoveTilePrompt(Session& session);
    void drawBake(Session& session);
    // Brush hover, ring and strokes in the 3D view; then updateObjects.
    void updateTool(Session& session);
    // zone_objects.cpp
    void drawAreas(Session& session);
    void drawSpawns(Session& session);
    void drawZoneObjects(Session& session);
    void drawObjectFields(Session& session);
    void drawScatter(Session& session);
    // A Scatter stroke in the 3D view: a dab on mouse down, another each time the brush moves its radius; one undo step.
    void updateScatter(Session& session, bool click);
    // Area, spawn and path tools and handle drags in the 3D view, then their overlays.
    void updateObjects(Session& session, bool click);
    void drawObjectOverlays(const Session& session) const;
    // Window pixel (ImGui points) of a ground point, if in front of the camera.
    std::optional<ImVec2> screen(const ZoneDoc& doc, float x, float z) const;
    void toolButton(const char* label, Tool tool);

    ZoneView& view_;
    Pending pending_;
    bool askUnsaved_ = false;
    bool openNewMap_ = false;
    std::string newFolder_, newName_;
    int newId_ = 0, newWidth_ = 2, newDepth_ = 2;
    bool quitConfirmed_ = false;
    size_t shownProblems_ = 0;
    bool focusProblems_ = false;
    std::string showPanel_;
    int showPanelFrames_ = 0;

    Tool tool_ = Tool::None;
    Brush brush_;
    float strength_ = 4.0f; // Raise/Lower: metres per second at the centre; Flatten: the fraction per second
    bool stroking_ = false;
    std::optional<Vec3> hover_; // the ground under the mouse
    TileKey tile_;             // import/export target: the last tile clicked
    float rangeMin_ = 0, rangeMax_ = 0;
    size_t rangeClips_ = 0;
    bool askRange_ = false;
    bool rangeEdited_ = false; // Min/Max typed but not applied: keep them
    NoiseParams noise_;
    Gradient gradient_;          // the stops shown; the manifest's while no edit is in progress
    bool colorsEditing_ = false; // a stop edit's undo step is open

    std::optional<size_t> area_;       // selected area (index into areas.json "areas")
    std::string spawn_;                // selected spawn id
    std::vector<Point> polygon_;       // AreaPolygon: the points so far
    std::optional<std::array<Point, 2>> rect_; // AreaRect: the drag's start and current corner
    Drag drag_;
    std::optional<TileKey> removeTile_; // asking to confirm its removal

    std::optional<ObjectRef> object_;   // selected zone object
    std::string placePrefab_;           // PlaceObject's prefab and type
    kast::map::ObjectType placeType_ = kast::map::ObjectType::Obstacle;
    Scatter scatter_;                   // the brush; its prefabs are chosen in the Objects panel
    bool scatterErase_ = false;         // the stroke removes visuals instead
    int scatterSeed_ = 1;
    ScatterRng scatterRng_{1};          // reseeded when the seed changes, so a seed replays the same strokes
    std::optional<Point> lastDab_;      // the current stroke's last dab (stroking_ covers the undo step)
    std::string dataText_;              // the selected interactable's "data" being typed
    uint32_t dataFor_ = 0;              // whose dataText_ it is (interactable id)
    std::string dataError_;
    bool mapbakeEdited_ = false;        // the exe path typed, saved once the field is left
};

} // namespace zone
