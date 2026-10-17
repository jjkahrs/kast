#pragma once

#include <project.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The Project Manager's recent-projects list: a JSON array in projects.json, most recent first (see
// docs/editor/project-manager.html). No ImGui; shared by the launcher and the editor's --project startup.

struct RecentProject {
    std::filesystem::path dir; // absolute, lexically normal
    std::string name;          // cached from project.kast when last opened
    int64_t lastOpened = 0;    // Unix seconds
};

// <SDL_GetPrefPath("Kast", "Editor")>/projects.json (%APPDATA%/Kast/Editor). Empty if SDL can't make that folder.
std::filesystem::path defaultRecentFile();
// The list in file; empty if it doesn't exist. An unreadable or invalid file is copied to <file>.bad and a warning is
// logged, then the list is empty.
std::vector<RecentProject> loadRecent(const std::filesystem::path& file);
// Writes the list (2-space indent), creating file's folder if needed. False + error on failure.
bool saveRecent(const std::filesystem::path& file, const std::vector<RecentProject>& list, std::string& error);
// Puts dir (made absolute and normal) at the top with this name and time, replacing any entry for the same folder
// (paths compared case-insensitively).
void bumpRecent(std::vector<RecentProject>& list, const std::filesystem::path& dir, std::string name, int64_t now);
// Why entry can't be opened (folder missing, or its project.kast missing or invalid), or empty if it can.
std::string recentProblem(const RecentProject& entry);
// loadRecent, bumpRecent(project, now), saveRecent: what every successful project open does.
bool recordRecent(const std::filesystem::path& file, const kast::Project& project, std::string& error);
