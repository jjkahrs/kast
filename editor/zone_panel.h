// The Zone tab: the open zone (ZoneSession) in one window of collapsing sections. A top bar (the map, Save Zone,
// Undo/Redo, the unsaved marker, the Scene view tool buttons), then Map (pick or create the map the scene's Terrain
// entity names, its manifest, its tiles), Terrain (zone_terrain.cpp: brushes, height range, heightmap PNGs, Generate,
// Height colors, the terrain material) and Problems (the last validation); the Areas, Spawns, Objects and Bake sections
// go between them. Read-only during Play and in prefab edit mode. Unsaved-changes prompt before the map changes (here
// or in the Inspector); scene-wide prompts and Ctrl+S go through FileActions::extra (main.cpp). The active tool gets the
// Scene view's left mouse through sceneInput (SceneViewport::input) and draws over it with drawSceneOverlay.
#pragma once

#include "document.h"
#include "viewport.h"
#include "zone_session.h"

#include <zone/terrain_edit.h>
#include <zone/zone_doc.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>

class ZonePanel {
public:
    static constexpr const char* kWindow = "Zone";
    // Scene view tools: while one is active, left mouse in the Scene view goes to it instead of entity picking and the
    // gizmo (sceneInput). Esc clears it.
    enum class Tool {
        None, Raise, Lower, Flatten, AreaRect, AreaPolygon, PlaceSpawn, PathPoints, SetSpawn, PlaceObject, Scatter
    };

    // The window and its modals; call once a frame after zone.update.
    void draw(Document& doc, ZoneSession& zone);
    // Saves the zone (ZoneSession::save); a blocked save opens Problems and brings the tab forward.
    bool save(ZoneSession& zone);
    // --zone-section: expand these sections ("Map", "Terrain", "Problems") and collapse the others, each the next time
    // it is drawn.
    void showSections(const std::vector<std::string>& names) { showSections_ = std::set(names.begin(), names.end()); }
    Tool tool() const { return tool_; }
    // --zone-tool: makes tool active, as its button does (ignored while there is no zone or the tab is read-only).
    void setTool(Tool tool) { tool_ = tool; }
    // "Raise", "SetSpawn", ...: the Tool of that name; nullopt if none.
    static std::optional<Tool> toolNamed(std::string_view name);

    // Before zone.update each frame: the first Terrain's map changed elsewhere (the Inspector) while the zone has
    // unsaved changes. Puts the old id back and asks first (the unsaved-zone prompt); the new id is applied after Save
    // or Don't Save, Cancel keeps the old map.
    void guardMapChange(Document& doc, const ZoneSession& zone);
    // SceneViewport::input: the active tool's Scene view mouse (the ground under it, clicks, strokes). True while a
    // tool is active: the left mouse is the zone's. Phase 5's handle drags (no tool) claim it here too.
    bool sceneInput(ZoneSession& zone, const SceneViewport::Input& input);
    // SceneViewport::overlay, after ZoneRender's: the active tool's overlay (the brush ring).
    void drawSceneOverlay(const ZoneSession& zone, ImDrawList* draw, const float* viewProj, float x, float y, float width,
                          float height) const;

private:
    // A map change waiting on the unsaved-zone prompt.
    struct Pending {
        enum class Kind { None, PickMap, NewMap } kind = Kind::None;
        int map = 0;
    };

    bool header(const char* label, const char* section, bool defaultOpen = false);
    void drawTopBar(ZoneSession& zone);
    void drawTerrainEntity(Document& doc, const ZoneSession& zone);
    void drawMap(Document& doc, ZoneSession& zone);
    void drawManifest(zone::ZoneDoc& z, const ZoneSession& zone);
    void drawTileGrid(zone::ZoneDoc& z, const kast::map::MapInfo& info);
    void drawProblems(const ZoneSession& zone);
    void drawUnsavedPrompt(Document& doc, ZoneSession& zone);
    void drawNewMapDialog(Document& doc, ZoneSession& zone);
    void drawRemoveTilePrompt(ZoneSession& zone);
    void toolButton(const char* label, Tool tool);
    // zone_terrain.cpp
    void drawTerrain(ZoneSession& zone);
    void drawGenerate(zone::ZoneDoc& z, const kast::map::MapInfo& info);
    void drawTerrainColors(zone::ZoneDoc& z);
    void drawHeightRangePrompt(ZoneSession& zone);
    void setHeightRange(zone::ZoneDoc& z, float min, float max);
    // A sculpt brush's Scene view mouse: Shift+click (Flatten) picks the height; a left drag is one undo step.
    void sculptInput(zone::ZoneDoc& z, const kast::map::MapInfo& info, bool click);
    // Ends a sculpt stroke (its undo step) once the mouse is up, the tool changed or the zone went away.
    void endStroke(ZoneSession& zone);
    // The heightmap file dialogs' answers (SDL may call back on another thread).
    static void onPngChosen(void* userdata, const char* const* files, int filter);
    void pollPngDialog(ZoneSession& zone);
    // Runs pending now, or after the prompt if the zone has unsaved changes.
    void request(Document& doc, ZoneSession& zone, Pending pending);
    void run(Document& doc, ZoneSession& zone, const Pending& pending);
    // Points the first Terrain entity (added if there is none) at map id; marks the scene dirty.
    void setTerrainMap(Document& doc, int id);

    std::optional<std::set<std::string>> showSections_;
    std::set<std::string> sectionsShown_; // the sections showSections_ has been applied to
    Tool tool_ = Tool::None;
    Pending pending_;
    bool askUnsaved_ = false;
    bool openNewMap_ = false;
    std::string newFolder_, newName_, newError_;
    int newId_ = 0, newWidth_ = 2, newDepth_ = 2;
    std::optional<kast::map::TileKey> removeTile_; // asking to confirm its removal
    size_t shownProblems_ = 0;
    bool openProblems_ = false;  // new problems: expand the section
    bool focusProblems_ = false; // a blocked save: bring the tab forward until it shows

    // Terrain
    zone::Brush brush_;
    float strength_ = 4.0f; // Raise/Lower: metres per second at the centre; Flatten: the fraction per second
    bool stroking_ = false;
    std::optional<zone::Vec3> hover_; // the ground under the mouse while a tool is active
    zone::TileKey tile_;              // heightmap import/export: the last tile clicked
    float rangeMin_ = 0, rangeMax_ = 0;
    size_t rangeClips_ = 0;
    bool askRange_ = false;
    bool rangeEdited_ = false; // Min/Max typed but not applied: keep them
    zone::NoiseParams noise_;
    zone::Gradient gradient_;    // the stops shown; the manifest's while no edit is in progress
    bool colorsEditing_ = false; // a stop edit's undo step is open
    bool pngDialogOpen_ = false;
    bool pngExport_ = false; // what the open dialog is for
    std::mutex pngMutex_;    // guards the answer:
    bool pngChosen_ = false;
    std::string pngPath_; // UTF-8; empty: cancelled
};
