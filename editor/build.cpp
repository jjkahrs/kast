#include "build.h"

#include <files.h>
#include <log.h>
#include <pak.h>
#include <scene.h>
#include <scene_io.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>

namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view kInvalidFileChars = R"(<>:"/\|?*)";
constexpr const char* kEditorDir = ".kast";
constexpr const char* kEditorFile = "editor.json";
constexpr const char* kReservedNames[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                          "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                          "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

std::string upper(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    return out;
}

bool isUnder(const std::filesystem::path& file, const std::filesystem::path& dir)
{
    return kast::toProjectPath(dir, file).has_value();
}

std::filesystem::path editorFile(const std::filesystem::path& projectRoot) { return projectRoot / kEditorDir / kEditorFile; }

} // namespace

std::string gameExeName(std::string_view projectName)
{
    std::string name;
    for (const char c : projectName) {
        const auto byte = static_cast<unsigned char>(c);
        name += byte < 0x20 || byte == 0x7f || kInvalidFileChars.find(c) != std::string_view::npos ? '_' : c;
    }
    const size_t first = name.find_first_not_of(' ');
    name.erase(0, first == std::string::npos ? name.size() : first);
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.empty())
        name = "Game";
    // Windows reserves the device names even with an extension: "CON.txt" is the console.
    const std::string base = upper(std::string_view(name).substr(0, name.find('.')));
    if (std::ranges::find(kReservedNames, base) != std::end(kReservedNames))
        name += '_';
    return name + ".exe";
}

bool shipsInPak(const std::string& projectPath)
{
    const std::filesystem::path relative(projectPath);
    return std::ranges::none_of(relative, [](const std::filesystem::path& part) { return part.string().starts_with('.'); });
}

std::vector<std::string> pakContents(const std::filesystem::path& root, const std::filesystem::path& outDir)
{
    std::vector<std::string> paths;
    const bool outInside = isUnder(outDir, root); // an outDir above the project holds it, so it excludes nothing
    // ponytail: walks into dot-directories (.git) and filters after; prune the walk if big repos make Build slow.
    for (const std::filesystem::path& file : kast::listFiles(root)) {
        const std::optional<std::string> path = kast::toProjectPath(root, file);
        if (path && !(outInside && isUnder(file, outDir)) && shipsInPak(*path))
            paths.push_back(*path);
    }
    return paths;
}

std::vector<std::string> projectScenes(const std::filesystem::path& root)
{
    std::vector<std::string> scenes;
    for (const std::filesystem::path& file : kast::listFiles(root)) {
        const std::optional<std::string> path = kast::toProjectPath(root, file);
        if (path && file.extension() == ".scene" && shipsInPak(*path))
            scenes.push_back(*path);
    }
    return scenes;
}

namespace {

// scene resolves inside the project and loads; error says why not.
bool sceneLoads(const std::filesystem::path& root, const std::string& scene, std::string& error)
{
    const std::optional<std::filesystem::path> file = kast::resolveProjectPath(root, scene, error);
    kast::Scene loaded;
    return file && kast::loadScene(file->string(), loaded, error);
}

} // namespace

bool checkStartScene(const kast::Project& project, std::string& error)
{
    if (!project.startScene) {
        error = std::format("Build failed: project '{}' has no startScene (set it in Build > Build Settings)", project.name);
        return false;
    }
    if (std::string reason; !sceneLoads(project.root, *project.startScene, reason)) {
        error = std::format("Build failed: startScene '{}': {}", *project.startScene, reason);
        return false;
    }
    return true;
}

bool setStartScene(kast::Project& project, const std::string& scene, std::string& error)
{
    if (std::string reason; !sceneLoads(project.root, scene, reason)) {
        error = std::format("Start Scene '{}': {}", scene, reason);
        return false;
    }
    kast::Project updated = project;
    updated.startScene = scene;
    if (!kast::saveProject(updated, error))
        return false;
    project = std::move(updated);
    kast::log::info(std::format("Start Scene set to '{}' in {}", scene, kast::kProjectFileName));
    return true;
}

bool buildGame(const kast::Project& project, const std::filesystem::path& outDir, const std::filesystem::path& playerDir,
               std::filesystem::path& exe, std::string& error)
{
    if (!checkStartScene(project, error))
        return false;
    const std::filesystem::path out = std::filesystem::absolute(outDir).lexically_normal();
    if (out == std::filesystem::absolute(project.root).lexically_normal()) {
        error = "Build failed: choose an output folder other than the project folder";
        return false;
    }
    std::vector<std::filesystem::path> sources{playerDir / kPlayerExeName};
    for (const char* dll : kPlayerDlls)
        sources.push_back(playerDir / dll);
    for (const std::filesystem::path& source : sources) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec)) {
            error = std::format("Build failed: '{}' is missing (build kast_player)", source.string());
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) {
        error = std::format("Build failed: cannot create '{}': {}", out.string(), ec.message());
        return false;
    }
    if (!kast::writePak(project.root, pakContents(project.root, out), out / kGamePakName, error)) {
        error = "Build failed: " + error;
        return false;
    }
    exe = out / gameExeName(project.name);
    for (const std::filesystem::path& source : sources) {
        const std::filesystem::path target = source.filename() == kPlayerExeName ? exe : out / source.filename();
        if (!std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec)) {
            error = std::format("Build failed: cannot write '{}': {}", target.string(), ec.message());
            return false;
        }
    }
    kast::log::info(std::format("Built '{}' to {}", project.name, out.string()));
    return true;
}

std::optional<std::filesystem::path> loadBuildFolder(const std::filesystem::path& projectRoot)
{
    std::string text;
    if (!kast::readFile(editorFile(projectRoot), text))
        return std::nullopt;
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("buildFolder") || !json["buildFolder"].is_string()) {
        kast::log::warn(std::format("Ignoring invalid '{}'", editorFile(projectRoot).string()));
        return std::nullopt;
    }
    return std::filesystem::path(json["buildFolder"].get<std::string>());
}

bool saveBuildFolder(const std::filesystem::path& projectRoot, const std::filesystem::path& folder, std::string& error)
{
    const std::filesystem::path file = editorFile(projectRoot);
    Json json;
    json["buildFolder"] = folder.generic_string();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << json.dump(2) << '\n';
    out.close();
    if (ec || !out) {
        error = std::format("Cannot write '{}'", file.string());
        return false;
    }
    return true;
}

bool launchGame(const std::filesystem::path& exe, std::string& error)
{
    const std::u8string path = exe.u8string(); // SDL takes UTF-8
    const char* args[] = {reinterpret_cast<const char*>(path.c_str()), nullptr};
    SDL_Process* process = SDL_CreateProcess(args, false);
    if (!process) {
        error = std::format("Cannot start '{}': {}", exe.string(), SDL_GetError());
        return false;
    }
    SDL_DestroyProcess(process); // only the handle: the game keeps running
    kast::log::info(std::format("Started '{}'", exe.string()));
    return true;
}
