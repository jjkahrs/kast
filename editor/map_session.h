// The editor's open map: the project map named by the edited scene's first Terrain entity (the rule TerrainStreamer
// follows in Play), loaded into a mapedit::MapDoc the Scene view renders (MapRender) and the Map tab edits.
#pragma once

#include "document.h"

#include <mapedit/map_io.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class MapSession {
public:
    // Follows doc while Play is stopped and no prefab is open (otherwise doc.scene is not the edited scene, so the map
    // stays as it is): scans the project's maps when the project changes, loads the map again when the project or the
    // first Terrain's map id changes, and unloads it when there is no project or Terrain entity. A load failure is logged
    // once and kept in error(); a loaded map is validated (problems()).
    void update(const Document& doc);
    // Rescans the project's maps and reloads the map from disk on the next update (unsaved map edits are lost).
    void reload() { loaded_.reset(); }
    // The open map's map id changed (its Id field, an undo of it) and the Terrain entity follows: no reload.
    void renumber(int map)
    {
        if (loaded_)
            loaded_->map = map;
    }

    // The map id the map (or error()) is for; nullopt until update tries one, and after reload().
    std::optional<int> loadedMap() const { return loaded_ ? std::optional(loaded_->map) : std::nullopt; }
    mapedit::MapDoc* doc() { return doc_ ? &*doc_ : nullptr; } // null: none loaded
    const mapedit::MapDoc* doc() const { return doc_ ? &*doc_ : nullptr; }
    bool dirty() const { return doc_ && doc_->dirty(); }
    // The project's maps (empty root: no project), .mat and .prefab files (project-relative) and NPC definition names
    // (mapedit::npc_names), scanned with the project.
    const mapedit::Project& project() const { return project_; }
    const std::vector<std::string>& materials() const { return materials_; }
    const std::vector<std::string>& prefabs() const { return prefabs_; }
    const std::vector<std::string>& npcs() const { return npcs_; }
    entt::entity terrainEntity() const { return terrain_; } // the first Terrain entity; null: none
    // Why the Terrain's map did not load (unknown id, unreadable or invalid files); empty when it loaded or there is
    // nothing to load.
    const std::string& error() const { return error_; }
    // The last validation (load, save): what would stop the world server loading the map; empty when it passed.
    const std::vector<mapedit::MapError>& problems() const { return problems_; }

    // After the Assets panel renamed or moved from to to (project-relative): the open map's map.json and objects, as
    // shown and as saved, name the new paths (the rename pass rewrote the files), so a later save writes no stale one.
    void remap(const std::string& from, const std::string& to);
    // mapedit::save_map on the open map (validation first: any problem writes nothing). Logs the result and each problem
    // to the Console. False with no map or when blocked.
    bool save();
    // Whether save sends the files it deletes (a removed tile's) to the Recycle Bin (default) or deletes them (--no-recycle).
    void setRecycle(bool recycle) { recycle_ = recycle; }
    // Scans root's maps, materials, prefabs and NPCs now, as update does when the project changes (map_cli, before the
    // first update).
    void openProject(const std::filesystem::path& root)
    {
        if (root != project_.root)
            scan(root);
    }
    // mapedit::load_map of maps/<folder> as the map, for that map's id: the caller points the Terrain entity at it before
    // the next update. False + error (map unchanged) if it does not load.
    bool open(const std::string& folder, std::string& error);
    // mapedit::validate on the open map into problems(), each logged to the Console. True when it passed.
    bool check();
    // mapedit::new_map in memory (written by the first save), opened as the map for map id: the caller points the Terrain
    // entity at id this frame. False + error (map unchanged) if new_map refuses.
    bool newMap(const std::string& folder, const std::string& name, uint16_t id, int width, int depth, std::string& error);

private:
    struct Key {
        std::filesystem::path root;
        int map = 0;
        bool operator==(const Key&) const = default;
    };

    void scan(const std::filesystem::path& root);

    std::optional<Key> loaded_; // what doc_ / error_ are for; nullopt: nothing tried yet
    mapedit::Project project_;
    std::vector<std::string> materials_, prefabs_, npcs_;
    std::optional<mapedit::MapDoc> doc_;
    entt::entity terrain_ = entt::null;
    std::string error_;
    std::vector<mapedit::MapError> problems_;
    bool recycle_ = true;
};
