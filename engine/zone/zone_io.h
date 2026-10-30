// The zone builder's file side: a project's maps, loading one into a ZoneDoc, validating it as the world server's
// loader does (kast_map's parsers plus the cross-file checks), and saving only what changed. No window.
#pragma once
#include "zone_doc.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zone {

// A map of the project as on disk: its manifest and areas parsed (for map ids and portal targets).
struct ProjectMap {
    std::string folder;
    kast::map::MapInfo info;
};

struct Project {
    std::filesystem::path root;
    std::vector<ProjectMap> maps;   // valid ones, sorted by folder
    std::vector<ZoneError> errors;  // maps that failed to parse
};

// The project's maps/<folder>/map.json (folders starting with '.' skipped, as the server does).
Project scan_project(const std::filesystem::path& root);

// The lowest map id no project map uses.
uint16_t next_map_id(const Project& project);

// Reads maps/<folder>: map.json, areas.json, every declared tile's heights (.r16 first, as the loaders) and every
// .server/spawns/, objects/ and .server/interactables/ <tx>_<tz>.json. nullopt with err set if a file is unreadable or
// the manifest is invalid.
std::optional<ZoneDoc> load_map(const Project& project, const std::string& folder, std::string& err);

// The most tiles a side a map's bounding box spans: new_map (the New Map dialog, --new-map) and add_tile (the Zone
// tab's tile grid, --add-tile) refuse more. kast_map and the server have no cap. Measured in tools/map_bench/results.md.
constexpr int kMaxMapTiles = 64;

// A new map in memory (nothing on disk until saved): tiles [0, 0, width - 1, depth - 1], flat, spawn in the middle
// of tile 0_0. nullopt with err set if folder is not a plain name or already exists, or width or depth is outside
// 1..kMaxMapTiles.
std::optional<ZoneDoc> new_map(const Project& project, const std::string& folder, const std::string& name, uint16_t id,
                               int width, int depth, std::string& err);

// Every problem that would stop the world server loading this map (an instance map's exit checked against the
// project's other maps); empty if none.
std::vector<ZoneError> validate(const Project& project, const ZoneDoc& doc);

// The files of tile t on disk (heights, spawn, objects and interactables files, navtile), project-relative: what
// removing it deletes on save.
std::vector<std::string> tile_files(const Project& project, const std::string& folder, TileKey t);

// Files of tiles the saved manifest has and the current one does not: heights, spawn and navtile files that save
// deletes. Project-relative.
std::vector<std::string> removed_tile_files(const Project& project, const ZoneDoc& doc);

// Validates, then writes what changed since the last save (heights as .r16; an edited tile's old .png is deleted) and
// deletes removed_tile_files and the spawn, objects and interactables files of tiles that no longer have one. Any validation error: writes nothing and returns them. recycle: deleted files go to the
// Recycle Bin (tests delete for good, to keep the bin clean). Rescans project on success.
std::vector<ZoneError> save_map(Project& project, ZoneDoc& doc, bool recycle = true);

// The "name" of every npcs/*.lua definition (`return { name = "Guard", ... }`), sorted and unique.
// ponytail: a regex over the file, not Lua; a name built in code is missed (the npc field also takes free text).
std::vector<std::string> npc_names(const std::filesystem::path& root);

// .r16 bytes of a tile: 257 * 257 little-endian uint16.
std::string encode_r16(const kast::map::Heights& heights);

} // namespace zone
