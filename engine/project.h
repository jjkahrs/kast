#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Projects: a directory marked by a project.kast file (JSON, see README "Projects"). Scene "systems" and
// kast.loadScene paths are relative to the project root, with '/' separators.
namespace kast {

constexpr const char* kProjectFileName = "project.kast";
constexpr int kProjectFileVersion = 1;

struct Project {
    std::filesystem::path root; // absolute directory holding project.kast
    std::string name;
    std::optional<std::string> startScene; // project-relative; stored for a future game client
};

// file is a project.kast path. Replaces out only on success; on failure error says why.
bool loadProject(const std::filesystem::path& file, Project& out, std::string& error);
// Writes project.root / project.kast (2-space indent, fixed key order).
bool saveProject(const Project& project, std::string& error);

// The nearest ancestor-or-self of dir holding a project.kast, or nullopt.
std::optional<std::filesystem::path> findProjectDir(const std::filesystem::path& dir);
// Walks up from dir and loads the project found. nullopt if none; an invalid project.kast is logged as a warning
// and treated as none.
std::optional<Project> discoverProject(const std::filesystem::path& dir);

// file as a '/'-separated path relative to root, or nullopt if it is outside root.
std::optional<std::string> toProjectPath(const std::filesystem::path& root, const std::filesystem::path& file);
// A project-relative path as an absolute path under root. nullopt + error if it is empty, absolute or escapes root.
std::optional<std::filesystem::path> resolveProjectPath(const std::filesystem::path& root, const std::string& path,
                                                        std::string& error);

} // namespace kast
