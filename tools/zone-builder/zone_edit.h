// Area, spawn, patrol path and zone object edits on a ZoneState's JSON, in place: keys the tool does not edit (an area's
// y, music, flags, anything unknown) stay as they were. Pure, no window; validation is zone_io's validate().
#pragma once
#include "zone_doc.h"

#include <objects.h>

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

// --- zone objects (objects/<tx>_<tz>.json "objects"; an interactable's server side in .server/interactables/) ---
// A tile whose list becomes empty loses its entry, so save deletes its file.

struct ObjectRef {
    TileKey tile;
    size_t index; // into that file's "objects"
    bool operator==(const ObjectRef&) const = default;
};

// Every object of the map, file by file.
std::vector<ObjectRef> all_objects(const ZoneState& state);
const Json& object_json(const ZoneState& state, ObjectRef ref);
Json& object_json(ZoneState& state, ObjectRef ref);
kast::map::ObjectType object_type(const Json& object); // visual for anything unknown
// One above the highest interactable id the map uses (objects or server entries); 1 if none.
uint32_t next_interactable_id(const ZoneState& state);
// The server entry with this id in tile's interactables file, or null.
Json* interactable_json(ZoneState& state, TileKey tile, uint32_t id);
const Json* interactable_json(const ZoneState& state, TileKey tile, uint32_t id);
// A new blocking object's shape: a 1 m box standing on its pos.
Json default_shape();
// Appends an object to the file of the tile pos is in: {"type", "prefab", "pos", "yaw", "scale"}, plus default_shape()
// for a blocking type and, for an interactable, the next id and its server entry (sync_interactable).
ObjectRef add_object(ZoneState& state, kast::map::ObjectType type, const std::string& prefab, const Vec3& pos, float yaw,
                     float scale);
// Moves an object; into another tile it moves to that tile's file, an interactable's server entry with it.
ObjectRef move_object(ZoneState& state, ObjectRef ref, const Vec3& pos);
// Removes an object, and an interactable's server entry.
void delete_object(ZoneState& state, ObjectRef ref);
// A blocking type gets default_shape() if it has none; an interactable gets an id and its server entry; one that stops
// being interactable loses both (its shape stays). Visuals keep a shape they had (unused, as the parsers ignore it).
void set_object_type(ZoneState& state, ObjectRef ref, kast::map::ObjectType type);
// Copies an interactable's id, pos, yaw, shape and name into its server entry, creating it with "range": 3 and
// "event": "<prefab file name>.activated" if missing. The server-only keys (range, event, data, unknown ones) stay.
void sync_interactable(ZoneState& state, ObjectRef ref);
// Renumbers an interactable, its server entry with it.
void set_interactable_id(ZoneState& state, ObjectRef ref, uint32_t id);

// --- scatter brush (visuals only) ---

struct Scatter {
    std::vector<std::string> prefabs; // one picked at random per object
    float radius = 8;
    int count = 10; // objects per dab
    float scaleMin = 0.8f, scaleMax = 1.2f;
    bool randomYaw = true;
};

// xorshift32: the same numbers from a seed on every compiler (std:: distributions are implementation-defined).
class ScatterRng {
public:
    explicit ScatterRng(uint32_t seed) : state_(seed ? seed : 0x9E3779B9u) {}
    float next(); // [0, 1)

private:
    uint32_t state_;
};

// One dab: brush.count visuals at random points of the circle at (x, z), on the ground (points off the declared tiles
// are skipped), with a random prefab, yaw (or 0) and scale. Returns how many were placed.
size_t scatter(const kast::map::MapInfo& info, ZoneState& state, const Scatter& brush, ScatterRng& rng, float x, float z);
// Removes every visual within radius of (x, z) on the ground plane. Returns how many.
size_t erase_visuals(ZoneState& state, float x, float z, float radius);

// [x, y, z] / [x, z] as the files write them (shortest float text).
Json vec3_json(const Vec3& v);
Json point_json(Point p);

} // namespace zone
