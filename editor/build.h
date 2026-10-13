#pragma once

#include <project.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Build: packages a project into a standalone game folder: <name>.exe (a copy of the prebuilt kast_player.exe next to
// the editor), game.pak (the project content, see pak.h) and the runtime DLLs the player loads.

constexpr const char* kGamePakName = "game.pak";
constexpr const char* kPlayerExeName = "kast_player.exe";
// What kast_player.exe imports beyond Windows and the MSVC runtime (dumpbin /dependents); vcpkg copies them next to it.
constexpr const char* kPlayerDlls[] = {"SDL3.dll", "lua51.dll"};

// The game exe's file name: the project name trimmed, each of < > : " / \ | ? * and control characters replaced by
// '_', trailing dots and spaces dropped, '_' appended to a reserved device name (CON, NUL, COM1, ...), "Game" if
// nothing is left; then ".exe". "Switch Test" -> "Switch Test.exe".
std::string gameExeName(std::string_view projectName);
// Whether a project-relative '/' path may ship in game.pak: false for dotfiles and anything in a dot-directory.
bool shipsInPak(const std::string& projectPath);
// What game.pak holds: every file under root (project-relative, sorted) that shipsInPak, except anything under outDir.
std::vector<std::string> pakContents(const std::filesystem::path& root, const std::filesystem::path& outDir);
// Every *.scene under root that shipsInPak: project-relative '/' paths, sorted (Build Settings' Start Scene choices).
std::vector<std::string> projectScenes(const std::filesystem::path& root);
// startScene is set, resolves inside the project and loads. False + error (prefixed "Build failed: ") if not.
bool checkStartScene(const kast::Project& project, std::string& error);
// Build Settings > Save: validates scene (inside the project, loads), then writes project.kast with it as startScene
// and updates project. Writes nothing and leaves project unchanged on failure (false + error).
bool setStartScene(kast::Project& project, const std::string& scene, std::string& error);
// Validates first (checkStartScene; outDir not the project root; the player and its
// DLLs present in playerDir), writing nothing on failure. Then creates outDir if needed and writes the exe, game.pak
// and the DLLs, overwriting those files only. Sets exe to the written exe. False + error on failure.
bool buildGame(const kast::Project& project, const std::filesystem::path& outDir, const std::filesystem::path& playerDir,
               std::filesystem::path& exe, std::string& error);

// The last Build output folder, per project and editor-local: <root>/.kast/editor.json ({"buildFolder": "..."}).
std::optional<std::filesystem::path> loadBuildFolder(const std::filesystem::path& projectRoot);
bool saveBuildFolder(const std::filesystem::path& projectRoot, const std::filesystem::path& folder, std::string& error);

// Starts exe as a separate process that keeps running after the editor exits. False + error if it can't start.
bool launchGame(const std::filesystem::path& exe, std::string& error);
