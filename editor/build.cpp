#include "build.h"

#include "assets.h"

#include <files.h>
#include <log.h>
#include <pak.h>
#include <scene.h>
#include <scene_io.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

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

bool isInvalidFileChar(char c)
{
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7f || kInvalidFileChars.find(c) != std::string_view::npos;
}

bool isReservedFileName(std::string_view name)
{
    // Windows reserves the device names even with an extension: "CON.txt" is the console.
    return std::ranges::find(kReservedNames, upper(name.substr(0, name.find('.')))) != std::end(kReservedNames);
}

std::string gameExeName(std::string_view projectName)
{
    std::string name;
    for (const char c : projectName)
        name += isInvalidFileChar(c) ? '_' : c;
    const size_t first = name.find_first_not_of(' ');
    name.erase(0, first == std::string::npos ? name.size() : first);
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.empty())
        name = "Game";
    if (isReservedFileName(name))
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

// scene resolves inside the project, ships in game.pak and loads; error says why not.
bool sceneLoads(const std::filesystem::path& root, const std::string& scene, std::string& error)
{
    const std::optional<std::filesystem::path> file = kast::resolveProjectPath(root, scene, error);
    if (!file)
        return false;
    if (const std::optional<std::string> path = kast::toProjectPath(root, *file); !path || !shipsInPak(*path)) {
        error = "it is a dotfile or in a dot-folder, which game.pak leaves out";
        return false;
    }
    kast::Scene loaded;
    return kast::loadScene(file->string(), loaded, error);
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
    const std::filesystem::path root = std::filesystem::absolute(project.root);
    if (isSameOrInside(out, root) && isSameOrInside(root, out)) { // ignoring case and a trailing separator
        error = "Build failed: choose an output folder other than the project folder";
        return false;
    }
    if (isSameOrInside(out, root)) { // its content leaves game.pak and the Assets panel, and Build overwrites it
        std::error_code ec;
        if (!std::filesystem::is_empty(out, ec) && !ec && !std::filesystem::exists(out / kGamePakName, ec)) {
            error = std::format("Build failed: '{}' is a project folder with other files in it; choose a new or empty "
                                "folder, or a previous build's (holding {})", utf8(out), kGamePakName);
            return false;
        }
        if (isSameOrInside(root / fromUtf8(*project.startScene), out)) {
            error = std::format("Build failed: startScene '{}' is in the output folder, which game.pak leaves out",
                                *project.startScene);
            return false;
        }
    }
    std::vector<std::filesystem::path> sources{playerDir / kPlayerExeName};
    for (const char* dll : kPlayerDlls)
        sources.push_back(playerDir / dll);
    for (const std::filesystem::path& source : sources) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec)) {
            error = std::format("Build failed: '{}' is missing (build kast_player)", utf8(source));
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) {
        error = std::format("Build failed: cannot create '{}': {}", utf8(out), ec.message());
        return false;
    }
    if (!kast::writePak(project.root, pakContents(project.root, out), out / kGamePakName, error)) {
        error = "Build failed: " + error;
        return false;
    }
    exe = out / fromUtf8(gameExeName(project.name));
    for (const std::filesystem::path& source : sources) {
        const std::filesystem::path target = source.filename() == kPlayerExeName ? exe : out / source.filename();
        if (!std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec)) {
            error = std::format("Build failed: cannot write '{}': {}", utf8(target), ec.message());
            return false;
        }
    }
    kast::log::info(std::format("Built '{}' to {}", project.name, utf8(out)));
    return true;
}

std::optional<std::filesystem::path> loadBuildFolder(const std::filesystem::path& projectRoot)
{
    std::string text;
    if (!kast::readFile(editorFile(projectRoot), text))
        return std::nullopt;
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("buildFolder") || !json["buildFolder"].is_string()) {
        kast::log::warn(std::format("Ignoring invalid '{}'", utf8(editorFile(projectRoot))));
        return std::nullopt;
    }
    return fromUtf8(json["buildFolder"].get<std::string>());
}

bool saveBuildFolder(const std::filesystem::path& projectRoot, const std::filesystem::path& folder, std::string& error)
{
    Json json;
    json["buildFolder"] = utf8(folder);
    return writeTextFile(editorFile(projectRoot), json.dump(2) + "\n", error);
}

SDL_Process* startProcess(const std::filesystem::path& exe, const std::vector<std::string>& args, std::string& error)
{
    const std::u8string path = exe.u8string(); // SDL takes UTF-8
    std::vector<const char*> argv{reinterpret_cast<const char*>(path.c_str())};
    for (const std::string& arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    SDL_Process* process = SDL_CreateProcess(argv.data(), false);
    if (!process) {
        error = std::format("Cannot start '{}': {}", utf8(exe), SDL_GetError());
        return nullptr;
    }
    kast::log::info(std::format("Started '{}'", utf8(exe)));
    return process;
}

bool launchProcess(const std::filesystem::path& exe, const std::vector<std::string>& args, std::string& error)
{
    SDL_Process* process = startProcess(exe, args, error);
    SDL_DestroyProcess(process); // only the handle: the game keeps running (null is ignored)
    return process != nullptr;
}
