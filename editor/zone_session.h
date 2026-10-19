// The editor's open zone: the project map named by the edited scene's first Terrain entity (the rule TerrainStreamer
// follows in Play), loaded into a zone::ZoneDoc the Scene view renders (ZoneRender) and the Zone tab edits.
#pragma once

#include "document.h"

#include <zone/zone_io.h>

#include <filesystem>
#include <optional>
#include <string>

class ZoneSession {
public:
    // Follows doc while Play is stopped and no prefab is open (otherwise doc.scene is not the edited scene, so the zone
    // stays as it is): loads the zone again when the project or the first Terrain's map id changes, and unloads it when
    // there is no project or Terrain entity. A load failure is logged once and kept in error().
    void update(const Document& doc);
    // Rescans the project's maps and reloads the zone from disk on the next update (unsaved zone edits are lost).
    void reload() { loaded_.reset(); }

    zone::ZoneDoc* zone() { return zone_ ? &*zone_ : nullptr; } // null: none loaded
    const zone::ZoneDoc* zone() const { return zone_ ? &*zone_ : nullptr; }
    const zone::Project& project() const { return project_; } // the maps scanned at the last load
    entt::entity terrainEntity() const { return terrain_; }    // the first Terrain entity; null: none
    // Why the Terrain's map did not load (unknown id, unreadable or invalid files); empty when it loaded or there is
    // nothing to load.
    const std::string& error() const { return error_; }

private:
    struct Key {
        std::filesystem::path root;
        int map = 0;
        bool operator==(const Key&) const = default;
    };

    std::optional<Key> loaded_; // what zone_ / error_ are for; nullopt: nothing tried yet
    zone::Project project_;
    std::optional<zone::ZoneDoc> zone_;
    entt::entity terrain_ = entt::null;
    std::string error_;
};
