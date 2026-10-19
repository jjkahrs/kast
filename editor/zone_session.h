// The editor's open zone: the project map named by the edited scene's first Terrain entity (the rule TerrainStreamer
// follows in Play), loaded into a zone::ZoneDoc the Scene view renders (ZoneRender) and the Zone tab edits.
#pragma once

#include "document.h"

#include <zone/zone_io.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ZoneSession {
public:
    // Follows doc while Play is stopped and no prefab is open (otherwise doc.scene is not the edited scene, so the zone
    // stays as it is): scans the project's maps when the project changes, loads the zone again when the project or the
    // first Terrain's map id changes, and unloads it when there is no project or Terrain entity. A load failure is logged
    // once and kept in error(); a loaded map is validated (problems()).
    void update(const Document& doc);
    // Rescans the project's maps and reloads the zone from disk on the next update (unsaved zone edits are lost).
    void reload() { loaded_.reset(); }
    // The open zone's map id changed (its Id field, an undo of it) and the Terrain entity follows: no reload.
    void renumber(int map)
    {
        if (loaded_)
            loaded_->map = map;
    }

    // The map id the zone (or error()) is for; nullopt until update tries one, and after reload().
    std::optional<int> loadedMap() const { return loaded_ ? std::optional(loaded_->map) : std::nullopt; }
    zone::ZoneDoc* zone() { return zone_ ? &*zone_ : nullptr; } // null: none loaded
    const zone::ZoneDoc* zone() const { return zone_ ? &*zone_ : nullptr; }
    bool dirty() const { return zone_ && zone_->dirty(); }
    // The project's maps (empty root: no project), .mat and .prefab files (project-relative) and NPC definition names
    // (zone::npc_names), scanned with the project.
    const zone::Project& project() const { return project_; }
    const std::vector<std::string>& materials() const { return materials_; }
    const std::vector<std::string>& prefabs() const { return prefabs_; }
    const std::vector<std::string>& npcs() const { return npcs_; }
    entt::entity terrainEntity() const { return terrain_; } // the first Terrain entity; null: none
    // Why the Terrain's map did not load (unknown id, unreadable or invalid files); empty when it loaded or there is
    // nothing to load.
    const std::string& error() const { return error_; }
    // The last validation (load, save): what would stop the world server loading the map; empty when it passed.
    const std::vector<zone::ZoneError>& problems() const { return problems_; }

    // zone::save_map on the open zone (validation first: any problem writes nothing). Logs the result and each problem
    // to the Console. False with no zone or when blocked.
    bool save(bool recycle = true);
    // zone::validate on the open zone into problems(), each logged to the Console. True when it passed.
    bool check();
    // zone::new_map in memory (written by the first save), opened as the zone for map id: the caller points the Terrain
    // entity at id this frame. False + error (zone unchanged) if new_map refuses.
    bool newMap(const std::string& folder, const std::string& name, uint16_t id, int width, int depth, std::string& error);

private:
    struct Key {
        std::filesystem::path root;
        int map = 0;
        bool operator==(const Key&) const = default;
    };

    void scan(const std::filesystem::path& root);

    std::optional<Key> loaded_; // what zone_ / error_ are for; nullopt: nothing tried yet
    zone::Project project_;
    std::vector<std::string> materials_, prefabs_, npcs_;
    std::optional<zone::ZoneDoc> zone_;
    entt::entity terrain_ = entt::null;
    std::string error_;
    std::vector<zone::ZoneError> problems_;
};
