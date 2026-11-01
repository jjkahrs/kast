#include "zone_scenes.h"

#include "files.h"

#include <zones.h>

#include <algorithm>
#include <format>

namespace kast {

std::string zoneScenePath(const std::string& folder, const std::string& zone)
{
    return std::format("maps/{}/zones/{}.scene", folder, zone);
}

std::optional<std::string> zoneSceneId(std::string_view fileName)
{
    constexpr std::string_view kExtension = ".scene";
    if (!fileName.ends_with(kExtension))
        return std::nullopt;
    fileName.remove_suffix(kExtension.size());
    if (!map::valid_zone_id(fileName))
        return std::nullopt;
    return std::string(fileName);
}

std::optional<ZoneSceneRef> parseZoneScenePath(std::string_view path)
{
    std::vector<std::string_view> parts;
    for (size_t start = 0; start <= path.size();) {
        const size_t end = std::min(path.find_first_of("/\\", start), path.size());
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    if (parts.size() != 4 || parts[0] != "maps" || parts[1].empty() || parts[2] != "zones")
        return std::nullopt;
    std::optional<std::string> zone = zoneSceneId(parts[3]);
    if (!zone)
        return std::nullopt;
    return ZoneSceneRef{std::string(parts[1]), std::move(*zone)};
}

ZoneSceneFiles listZoneScenes(const std::filesystem::path& mapFolder)
{
    ZoneSceneFiles files;
    const std::filesystem::path dir = (mapFolder / "zones").lexically_normal();
    for (const std::filesystem::path& file : listFiles(dir)) {
        const std::u8string name = file.filename().u8string();
        const std::string text(name.begin(), name.end());
        if (file.parent_path().lexically_normal() != dir || text.ends_with(".meta"))
            continue; // as listTileScenes
        if (std::optional<std::string> zone = zoneSceneId(text))
            files.zones.push_back(std::move(*zone));
        else
            files.badNames.push_back(text);
    }
    std::ranges::sort(files.zones);
    return files;
}

} // namespace kast
