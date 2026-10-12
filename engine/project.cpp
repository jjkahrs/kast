#include "project.h"

#include "log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kProjectKeys[] = {"version", "name", "startScene"};

std::string validate(const Json& json)
{
    if (!json.is_object())
        return "must be a JSON object";
    for (const char* key : kProjectKeys)
        if (!json.contains(key))
            return std::format("missing \"{}\"", key);
    for (const auto& [key, value] : json.items())
        if (std::ranges::find(kProjectKeys, key) == std::end(kProjectKeys))
            return std::format("unknown key \"{}\"", key);
    if (!json["version"].is_number_integer() || json["version"].get<int>() != kProjectFileVersion)
        return std::format("unsupported \"version\" {} (expected {})", json["version"].dump(), kProjectFileVersion);
    if (!json["name"].is_string() || json["name"].get<std::string>().empty())
        return "\"name\" must be a non-empty string";
    if (!json["startScene"].is_string() && !json["startScene"].is_null())
        return "\"startScene\" must be a string or null";
    return {};
}

// ponytail: lexical comparison, so paths must agree on case (e.g. the drive letter); canonicalize if that bites.
std::filesystem::path normalized(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

bool escapes(const std::filesystem::path& relative)
{
    return relative.empty() || *relative.begin() == "..";
}

} // namespace

bool loadProject(const std::filesystem::path& file, Project& out, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = std::format("Cannot open project '{}'", file.string());
        return false;
    }
    std::string reason;
    try {
        const Json json = Json::parse(in);
        reason = validate(json);
        if (reason.empty()) {
            out = Project{.root = normalized(file).parent_path(), .name = json["name"].get<std::string>()};
            if (json["startScene"].is_string())
                out.startScene = json["startScene"].get<std::string>();
            return true;
        }
    } catch (const Json::exception& e) {
        reason = e.what();
    }
    error = std::format("Invalid project '{}': {}", file.string(), reason);
    return false;
}

bool saveProject(const Project& project, std::string& error)
{
    Json json;
    json["version"] = kProjectFileVersion;
    json["name"] = project.name;
    json["startScene"] = project.startScene ? Json(*project.startScene) : Json(nullptr);
    const std::filesystem::path file = project.root / kProjectFileName;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << json.dump(2) << '\n';
    out.close();
    if (!out) {
        error = std::format("Cannot write project '{}'", file.string());
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> findProjectDir(const std::filesystem::path& dir)
{
    for (std::filesystem::path current = normalized(dir);; current = current.parent_path()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(current / kProjectFileName, ec))
            return current;
        if (current == current.parent_path())
            return std::nullopt;
    }
}

std::optional<Project> discoverProject(const std::filesystem::path& dir)
{
    const std::optional<std::filesystem::path> found = findProjectDir(dir);
    if (!found)
        return std::nullopt;
    Project project;
    if (std::string error; !loadProject(*found / kProjectFileName, project, error)) {
        log::warn(std::format("{}; no project is active", error));
        return std::nullopt;
    }
    return project;
}

std::optional<std::string> toProjectPath(const std::filesystem::path& root, const std::filesystem::path& file)
{
    const std::filesystem::path relative = normalized(file).lexically_relative(normalized(root));
    if (escapes(relative) || relative == ".")
        return std::nullopt;
    return relative.generic_string();
}

std::optional<std::filesystem::path> resolveProjectPath(const std::filesystem::path& root, const std::string& path,
                                                        std::string& error)
{
    const std::filesystem::path relative(path);
    if (path.empty()) {
        error = "path is empty";
    } else if (relative.has_root_name() || relative.has_root_directory()) {
        error = std::format("'{}' must be relative to the project root", path);
    } else if (const std::filesystem::path file = (normalized(root) / relative).lexically_normal();
               escapes(file.lexically_relative(normalized(root)))) {
        error = std::format("'{}' is outside the project", path);
    } else {
        return file;
    }
    return std::nullopt;
}

} // namespace kast
