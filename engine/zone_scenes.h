#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Zone scenes (docs/editor/map-builder.html#zones): each zone of a map (zones.json, services/world/map/zones.h) may
// have its own scene file, maps/<folder>/zones/<zoneId>.scene (project-relative), its entities in world coordinates,
// loaded additively like tile scenes (tile_scenes.h) and never the active scene. Pure: the paths and the folder
// listing, no window and no Scene.
namespace kast {

// "maps/<folder>/zones/<zoneId>.scene"
std::string zoneScenePath(const std::string& folder, const std::string& zone);
// The zone id a "<zoneId>.scene" file name names (map::valid_zone_id); nullopt for any other name.
std::optional<std::string> zoneSceneId(std::string_view fileName);

struct ZoneSceneRef {
    std::string folder; // maps/<folder>
    std::string zone;
};
// A project-relative "maps/<folder>/zones/<zoneId>.scene" ('/' or '\'); nullopt for any other path.
std::optional<ZoneSceneRef> parseZoneScenePath(std::string_view path);

// A map folder's zones/: the zone ids with a scene file (sorted) and the names of the other files there.
struct ZoneSceneFiles {
    std::vector<std::string> zones;
    std::vector<std::string> badNames;
};
ZoneSceneFiles listZoneScenes(const std::filesystem::path& mapFolder);

} // namespace kast
