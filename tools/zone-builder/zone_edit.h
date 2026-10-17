// Area, spawn and patrol path edits on a ZoneState's JSON, in place: keys the tool does not edit (an area's y, music,
// flags, anything unknown) stay as they were. Pure, no window; validation is zone_io's validate().
#pragma once
#include "zone_doc.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace zone {

using Point = std::array<float, 2>; // x, z

// --- areas (areas.json "areas") ---

// The areas list, creating {"version": 1, "areas": []} if the map has no areas.json yet.
Json& areas_list(ZoneState& state);
// Index of the area with id, if any.
std::optional<size_t> find_area(const ZoneState& state, const std::string& id);
// base if no area uses it, else base_2, base_3...
std::string unique_area_id(const ZoneState& state, const std::string& base);
// Appends an area: a rect [x0, z0, x1, z1] (corners in any order) or a polygon. Returns its index.
size_t add_rect_area(ZoneState& state, const std::string& id, Point a, Point b, int priority);
size_t add_polygon_area(ZoneState& state, const std::string& id, const std::vector<Point>& points, int priority);
// An area's outline: a rect's 4 corners (x0z0, x1z0, x1z1, x0z1) or the polygon's points.
std::vector<Point> area_points(const Json& area);
// Moves one outline vertex. A rect stays a rect: its corner moves, dragging the two edges that meet there.
void move_area_vertex(Json& area, size_t vertex, Point to);
// Moves the whole outline by (dx, dz).
void move_area(Json& area, float dx, float dz);
// Renames an area, and its children's "parent" with it.
void rename_area(ZoneState& state, size_t index, const std::string& id);
// Removes an area; its children lose their parent (they become top level).
void delete_area(ZoneState& state, size_t index);

// --- spawns (.server/spawns/<tx>_<tz>.json) ---

struct SpawnRef {
    TileKey tile;
    size_t index; // into that file's "spawns"
    bool operator==(const SpawnRef&) const = default;
};

// Every spawn of the map, file by file.
std::vector<SpawnRef> all_spawns(const ZoneState& state);
const Json& spawn_json(const ZoneState& state, SpawnRef ref);
Json& spawn_json(ZoneState& state, SpawnRef ref);
std::optional<SpawnRef> find_spawn(const ZoneState& state, const std::string& id);
std::string unique_spawn_id(const ZoneState& state, const std::string& base);
// The tile's spawn file, creating {"version": 2, "paths": {}, "spawns": []} if there is none.
Json& spawn_file(ZoneState& state, TileKey tile);
// Appends a spawn to the file of the tile pos stands in.
SpawnRef add_spawn(ZoneState& state, const std::string& id, const std::string& npc, const Vec3& pos, float heading,
                   float respawnSec);
// Moves a spawn. Into another tile it moves to that tile's file, with its path: the path is copied there (renamed if
// the name is taken by a different path) and removed from the old file once no spawn there uses it.
SpawnRef move_spawn(ZoneState& state, SpawnRef ref, const Vec3& pos);
void delete_spawn(ZoneState& state, SpawnRef ref);

// --- patrol paths (a spawn file's "paths") ---

// Adds a path to ref's file (name made unique there) and assigns it to the spawn. Returns the name used.
std::string add_path(ZoneState& state, SpawnRef ref, const std::string& name, const std::vector<Vec3>& points,
                     float wait, bool loop);
// Removes a path from a file and unassigns it from the file's spawns.
void delete_path(ZoneState& state, TileKey tile, const std::string& name);
// The spawn's route: its file's path named by its "path", or null if it has none (or names a missing one).
Json* spawn_path(ZoneState& state, SpawnRef ref);
// Appends a point to the spawn's route, creating "<id>_path" from the spawn's position if it has none.
void add_path_point(ZoneState& state, SpawnRef ref, const Vec3& point);

// [x, y, z] / [x, z] as the files write them (shortest float text).
Json vec3_json(const Vec3& v);
Json point_json(Point p);

} // namespace zone
