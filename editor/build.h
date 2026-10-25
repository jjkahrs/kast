#pragma once

#include <project.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Process;

// Build: packages a project into a standalone game folder: <name>.exe (a copy of the prebuilt kast_player.exe next to
// the editor), game.pak (the project content, see pak.h) and the runtime DLLs the player loads.

constexpr const char* kGamePakName = "game.pak";
constexpr const char* kPlayerExeName = "kast_player.exe";
// What kast_player.exe imports beyond Windows and the MSVC runtime, directly (OpenSSL for kast.net, meshoptimizer for
// lightmapped LODs) or through RmlUi -> FreeType (dumpbin /dependents); vcpkg copies them next to it. No Assimp: the player loads cooked models (model.h).
// vcpkg's debug builds of the FreeType chain carry a 'd' suffix.
#ifdef NDEBUG
constexpr const char* kPlayerDlls[] = {"SDL3.dll",      "lua51.dll",     "rmlui.dll",        "freetype.dll",
                                       "zlib1.dll",     "bz2.dll",       "libpng16.dll",     "brotlidec.dll",
                                       "brotlicommon.dll", "libssl-3-x64.dll", "libcrypto-3-x64.dll", "meshoptimizer.dll", "pugixml.dll"};
#else
constexpr const char* kPlayerDlls[] = {"SDL3.dll",      "lua51.dll",     "rmlui.dll",        "freetyped.dll",
                                       "zlibd1.dll",    "bz2d.dll",      "libpng16d.dll",    "brotlidec.dll",
                                       "brotlicommon.dll", "libssl-3-x64.dll", "libcrypto-3-x64.dll", "meshoptimizer.dll", "pugixml.dll"};
#endif

// c can't appear in a Windows file name: one of < > : " / \ | ? * or a control character.
bool isInvalidFileChar(char c);
// name up to its first '.' is a reserved Windows device name (CON, NUL, COM1, ...), ignoring case.
bool isReservedFileName(std::string_view name);
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
// startScene is set, resolves inside the project, shipsInPak and loads. False + error (prefixed "Build failed: ") if not.
bool checkStartScene(const kast::Project& project, std::string& error);
// Build Settings > Save: validates scene (inside the project, shipsInPak, loads), then writes project.kast with it as startScene
// and updates project. Writes nothing and leaves project unchanged on failure (false + error).
bool setStartScene(kast::Project& project, const std::string& scene, std::string& error);
// online is valid for project: onlineConfigError, and while enabled a caFile that exists, holds a valid PEM certificate
// and shipsInPak. False + error (prefixed "Online Services: ") if not. Reads, never writes.
bool checkOnlineConfig(const kast::Project& project, const kast::OnlineConfig& online, std::string& error);
// Online Services > Save: checkOnlineConfig, then writes project.kast with online and updates project. Writes nothing
// and leaves project unchanged on failure (false + error).
bool setOnlineConfig(kast::Project& project, const kast::OnlineConfig& online, std::string& error);
// Display & GUI Scaling > Save (and --gui-settings-set): writes project.kast with gui (validated as loading would) and
// updates project. Writes nothing and returns false + error if gui is invalid or the write fails.
bool setGuiConfig(kast::Project& project, const kast::GuiConfig& gui, std::string& error);
// Validates first (checkStartScene; checkOnlineConfig; outDir not the project root; an outDir inside the project new, empty or holding
// game.pak, and not holding startScene; the player and its DLLs present in playerDir), writing nothing on failure. Then creates outDir if needed and writes the exe, game.pak
// and the DLLs, overwriting those files only. Sets exe to the written exe. False + error on failure.
bool buildGame(const kast::Project& project, const std::filesystem::path& outDir, const std::filesystem::path& playerDir,
               std::filesystem::path& exe, std::string& error);

// The last Build output folder, per project and editor-local: <root>/.kast/editor.json ({"buildFolder": "..."}).
std::optional<std::filesystem::path> loadBuildFolder(const std::filesystem::path& projectRoot);
bool saveBuildFolder(const std::filesystem::path& projectRoot, const std::filesystem::path& folder, std::string& error);

// Starts exe with args (UTF-8) as a separate process; it inherits the console. Returns its handle (the caller
// SDL_DestroyProcess-es it; the process keeps running), or null + error if it can't start.
SDL_Process* startProcess(const std::filesystem::path& exe, const std::vector<std::string>& args, std::string& error);
// startProcess without keeping the handle: the process keeps running after the editor exits.
bool launchProcess(const std::filesystem::path& exe, const std::vector<std::string>& args, std::string& error);
