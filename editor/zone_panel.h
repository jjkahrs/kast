// The Zone tab: the open zone (ZoneSession) in one window of collapsing sections. A top bar (the map, Save Zone,
// Undo/Redo, the unsaved marker, the Scene view tool buttons), then Map (pick or create the map the scene's Terrain
// entity names, its manifest, its tiles) and Problems (the last validation); the Terrain, Areas, Spawns, Objects and
// Bake sections go between them. Read-only during Play and in prefab edit mode. Unsaved-changes prompt before the map
// changes; scene-wide prompts and Ctrl+S go through FileActions::extra (main.cpp).
#pragma once

#include "document.h"
#include "zone_session.h"

#include <zone/zone_doc.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>

class ZonePanel {
public:
    static constexpr const char* kWindow = "Zone";
    // Scene view tools: while one is active, left mouse in the Scene view goes to it instead of entity picking (the
    // Scene view routing and the tools' buttons come with their sections). Esc clears it.
    enum class Tool {
        None, Raise, Lower, Flatten, AreaRect, AreaPolygon, PlaceSpawn, PathPoints, SetSpawn, PlaceObject, Scatter
    };

    // The window and its modals; call once a frame after zone.update.
    void draw(Document& doc, ZoneSession& zone);
    // Saves the zone (ZoneSession::save); a blocked save opens Problems and brings the tab forward.
    bool save(ZoneSession& zone);
    // --zone-section: expand this section ("Map", "Problems") the next time it is drawn.
    void openSection(std::string name) { openSections_.insert(std::move(name)); }
    Tool tool() const { return tool_; }

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
    // Runs pending now, or after the prompt if the zone has unsaved changes.
    void request(Document& doc, ZoneSession& zone, Pending pending);
    void run(Document& doc, ZoneSession& zone, const Pending& pending);
    // Points the first Terrain entity (added if there is none) at map id; marks the scene dirty.
    void setTerrainMap(Document& doc, int id);

    std::set<std::string> openSections_;
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
};
