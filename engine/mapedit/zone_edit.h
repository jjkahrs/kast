// Zone edits on a MapState's zones.json (services/world/map/zones.h, docs/editor/map-builder.html#zones), in place: a
// zone's keys the tool does not edit (anything unknown) stay as they were. A map without zones.json has one implicit
// zone; the first zone edit writes it out (zones_list). Pure, no window; save-blocking problems come from map_zones
// (map_io's validate()).
#pragma once
#include "terrain_edit.h" // TileSet

#include <zones.h>

#include <optional>
#include <string>
#include <vector>

namespace mapedit {

// The order a tile's edge neighbours are tried in: -x, +x, -z, +z. A tile joining the map (add_tile) gets the zone of
// the first one in a zone, else the first zone; a deleted zone's tiles go to the zone of the first neighbour (of its
// tiles, sorted z then x) in another zone.
constexpr TileKey kZoneNeighbours[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

// The map's zones as the client reads them (zones.json's, else the implicit one), byTile filled. problems: what blocks
// the save, each naming its zone (the file's structure, then kast::map::validate_zones); none for the implicit zone.
kast::map::Zones map_zones(const kast::map::MapInfo& info, const MapState& state, const std::string& folder,
                           std::vector<std::string>* problems = nullptr);
// zones.json's "zones", creating {"version": 1, "zones": [the implicit zone]} if the map has no zones.json yet.
Json& zones_list(const kast::map::MapInfo& info, MapState& state, const std::string& folder);
// Index of the zone with id, if any.
std::optional<size_t> find_zone(const MapState& state, const std::string& id);
// base if no zone uses it, else base_2, base_3...
std::string unique_zone_id(const MapState& state, const std::string& base);
// The tiles of a zone's "tiles" rects. ponytail: a malformed rect, or one over kMaxMapTiles^2 tiles, is skipped (the
// validation names it); an edit that rewrites the zone's tiles drops it.
TileSet zone_tiles(const Json& zone);

// Appends a zone with no tiles yet (a problem until painted); name empty: none (it defaults to the id). Its index.
size_t add_zone(const kast::map::MapInfo& info, MapState& state, const std::string& folder, const std::string& id,
                const std::string& name);
// Moves the declared tiles of `tiles` into zone `zone`, out of the zones they were in. How many moved.
size_t paint_zone(const kast::map::MapInfo& info, MapState& state, size_t zone, const TileSet& tiles);
// paint_zone, refused if it would add a map_zones problem (a zone in pieces, or left with no tiles): then state is as it
// was, moved is 0 and the problem is returned. The editor's paint strokes (tile grid, Scene view) go through this.
std::optional<std::string> paint_zone_checked(const kast::map::MapInfo& info, MapState& state, const std::string& folder,
                                              size_t zone, const TileSet& tiles, size_t& moved);
// Renames a zone; its scene file (zones/<id>.scene) follows on save (zoneRenames).
void rename_zone(MapState& state, size_t zone, const std::string& id);
// Removes a zone. Its tiles go to an edge neighbour's zone (kZoneNeighbours), whose id is returned; with none they are
// in no zone (a problem blocking the save). Its scene file stays (an orphan).
std::optional<std::string> delete_zone(MapState& state, size_t zone);
// The id whose scene file (zones/<id>.scene) is zone's: the id it had at the last save if renamed since.
std::string zone_scene_id(const MapState& state, const std::string& zone);

// add_tile / remove_tile keep zones.json in step (nothing without one): a new tile joins its first edge neighbour's
// zone (else the first zone); a removed tile leaves its zone, which may then be split or empty (problems).
void zones_add_tile(MapState& state, TileKey t);
void zones_remove_tile(MapState& state, TileKey t);

} // namespace mapedit
