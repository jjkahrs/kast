#include "recent_projects.h"

#include <files.h>
#include <log.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>

namespace {

std::filesystem::path normalized(const std::filesystem::path& dir)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
    std::filesystem::path path = (ec ? dir : absolute).lexically_normal();
    if (!path.has_filename() && path.has_parent_path() && path != path.root_path())
        path = path.parent_path(); // "C:\game\" -> "C:\game"
    return path;
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) // Windows: case-insensitive
{
    return std::ranges::equal(a.native(), b.native(),
                              [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path fromUtf8(const std::string& text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

} // namespace

std::filesystem::path defaultRecentFile()
{
    char* dir = SDL_GetPrefPath("Kast", "Editor");
    if (!dir) {
        kast::log::error(std::format("Cannot find the recent projects folder: {}", SDL_GetError()));
        return {};
    }
    const std::filesystem::path file = fromUtf8(dir) / "projects.json";
    SDL_free(dir);
    return file;
}

std::vector<RecentProject> loadRecent(const std::filesystem::path& file)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return {};
    std::vector<RecentProject> list;
    try {
        std::ifstream in(file, std::ios::binary);
        const nlohmann::json json = nlohmann::json::parse(in);
        for (const nlohmann::json& entry : json.get_ref<const nlohmann::json::array_t&>())
            list.push_back({normalized(fromUtf8(entry.at("path").get<std::string>())), entry.at("name").get<std::string>(),
                            entry.at("lastOpened").get<int64_t>()});
        return list;
    } catch (const nlohmann::json::exception& e) {
        std::filesystem::path bad = file;
        bad += ".bad";
        std::filesystem::copy_file(file, bad, std::filesystem::copy_options::overwrite_existing, ec);
        kast::log::warn(std::format("Recent projects list '{}' is invalid ({}); {}, starting with an empty list",
                                    file.string(), e.what(), ec ? "could not back it up" : "backed up to " + bad.string()));
        return {};
    }
}

bool saveRecent(const std::filesystem::path& file, const std::vector<RecentProject>& list, std::string& error)
{
    nlohmann::ordered_json json = nlohmann::ordered_json::array();
    for (const RecentProject& entry : list)
        json.push_back({{"path", utf8(entry.dir)}, {"name", entry.name}, {"lastOpened", entry.lastOpened}});
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!kast::writeFileAtomic(file, json.dump(2) + '\n')) {
        error = std::format("Cannot write the recent projects list '{}'", file.string());
        return false;
    }
    return true;
}

void bumpRecent(std::vector<RecentProject>& list, const std::filesystem::path& dir, std::string name, int64_t now)
{
    const std::filesystem::path path = normalized(dir);
    std::erase_if(list, [&](const RecentProject& entry) { return samePath(entry.dir, path); });
    list.insert(list.begin(), {path, std::move(name), now});
}

std::string recentProblem(const RecentProject& entry)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(entry.dir, ec))
        return "Folder not found";
    kast::Project project;
    std::string error;
    return kast::loadProject(entry.dir / kast::kProjectFileName, project, error) ? "" : error;
}

bool recordRecent(const std::filesystem::path& file, const kast::Project& project, std::string& error)
{
    std::vector<RecentProject> list = loadRecent(file);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    bumpRecent(list, project.root, project.name, now.count());
    return saveRecent(file, list, error);
}
