#include "build.h"

#include "assets.h"
#include "model_importer.h"
#include "shader_compiler.h"
#include "texture_cooker.h"

#include <files.h>
#include <gui_prefabs.h>
#include <log.h>
#include <model.h>
#include <pak.h>
#include <renderer.h>
#include <scene.h>
#include <scene_io.h>
#include <shader.h>
#include <shader_graph.h>
#include <sky.h>
#include <texture_cook.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace {

using Json = nlohmann::ordered_json;
using TextureUsages = std::map<std::string, std::set<kast::TextureUsage>>; // by texture path

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
    return kast::loadScene(file->string(), loaded, error, nullptr, root);
}

// Compiles every .kshader and .kshadergraph among paths (game.pak's) into bytecode entries at kast::shaderPakPath, which
// the player loads instead of compiling (a graph's keyed by its generated text, which the player generates again from
// the shipped graph). Every shipped one, not only those materials name: Lua can switch a material's shader. False +
// error naming the first file that fails and its line (a graph's: its node). sources: each one as parsed, by path.
bool compileShipped(const std::filesystem::path& root, const std::vector<std::string>& paths,
                    std::map<std::string, std::string>& bytecode, std::map<std::string, kast::ShaderSource>& sources,
                    std::string& error)
{
    const std::string version = shaderToolsVersion();
    for (const std::string& path : paths) {
        if (!path.ends_with(".kshader") && !kast::isShaderGraphPath(path))
            continue;
        std::string text, reason;
        kast::ShaderSource source;
        kast::CompiledShader compiled;
        bool cached = false;
        if (!kast::readFile(root / fromUtf8(path), text))
            reason = "cannot read the file";
        else if (kast::shaderFileText(path, text, reason) && kast::parseShader(text, source, reason) &&
                 !kast::compileShaderStages(source, text, compileShader, version, root / kEditorDir / "shader_cache",
                                            compiled, cached, reason) &&
                 kast::isShaderGraphPath(path))
            reason = kast::mapGraphErrors(text, reason);
        if (!reason.empty()) {
            error = std::format("Build failed: shader '{}': {}", path, reason);
            return false;
        }
        for (const kast::ShaderStage stage : kast::shaderStages(source)) {
            const std::vector<uint8_t>& code = compiled.stage(stage);
            bytecode[kast::shaderPakPath(text, stage)] = std::string(code.begin(), code.end());
        }
        kast::log::info(std::format("Compiled shader '{}' for game.pak{}", path, cached ? " (cache hit)" : ""));
        sources[path] = std::move(source);
    }
    // A material its shader can't serve (SCREEN_TEXTURE / DEPTH_TEXTURE when opaque or mask) would render magenta.
    for (const std::string& path : paths) {
        kast::Material material;
        std::string reason;
        if (!path.ends_with(".mat") || !kast::loadMaterial(root / fromUtf8(path), material, reason))
            continue; // a broken .mat is the renderer's to report, as before
        const auto it = sources.find(material.shader);
        if (it != sources.end() && !kast::checkShaderAlphaMode(material, it->second, reason)) {
            error = std::format("Build failed: material '{}': {}", path, reason);
            return false;
        }
    }
    return true;
}

// Cooks every model file among paths (game.pak's) into a .kmodel entry at kast::modelPakPath, through the editor's model
// cache, and takes the model files and the sidecars they import from (.bin, .mtl) out of paths: the player loads only
// cooked models (it has no Assimp). Every shipped one, not only those scenes name: Lua can load a model by path. False +
// error naming the first file that fails. Adds the usages of the textures their materials sample to usages, and their
// embedded images ("<model>#<i>") to embedded.
bool cookShippedModels(const std::filesystem::path& root, std::vector<std::string>& paths,
                       std::map<std::string, std::string>& cooked, TextureUsages& usages,
                       std::map<std::string, std::string>& embedded, std::string& error)
{
    const std::string version = modelImporterVersion();
    std::set<std::string> sources;
    for (const std::string& path : paths) {
        if (!kast::isModelFile(path))
            continue;
        kast::CookedModel model;
        std::string reason;
        if (!kast::cookModel(root, path, importModel, version, root / kEditorDir / "model_cache", model, reason)) {
            error = std::format("Build failed: model '{}': {}", path, reason);
            return false;
        }
        for (const kast::ModelMaterial& material : model.model->materials)
            for (const auto& [texture, usage] : kast::materialTextures(material.material, nullptr))
                usages[texture].insert(usage);
        for (size_t i = 0; i < model.model->textures.size(); ++i)
            embedded[std::format("{}#{}", path, i)] = model.model->textures[i];
        cooked[kast::modelPakPath(path)] = std::move(model.bytes);
        sources.insert(path);
        sources.insert(model.sidecars.begin(), model.sidecars.end());
        kast::log::info(std::format("Cooked model '{}' for game.pak{}", path, model.cached ? " (cache hit)" : ""));
    }
    std::erase_if(paths, [&](const std::string& path) { return sources.contains(path); });
    return true;
}

bool isImageFile(const std::string& path)
{
    const std::string extension = upper(std::filesystem::path(fromUtf8(path)).extension().string());
    return extension == ".PNG" || extension == ".JPG" || extension == ".JPEG";
}

// Adds what a scene or prefab's JSON samples: particle textures ("texture") as Color, and each sky's six faces.
void sceneTextures(const Json& json, TextureUsages& usages, std::set<std::array<std::string, 6>>& skies)
{
    if (json.is_array())
        for (const Json& item : json)
            sceneTextures(item, usages, skies);
    if (!json.is_object())
        return;
    for (const auto& [key, value] : json.items()) {
        if (key == "texture" && value.is_string()) {
            usages[value.get<std::string>()].insert(kast::TextureUsage::Color);
        } else if (key == "faces" && value.is_object()) {
            std::array<std::string, 6> faces;
            for (size_t i = 0; i < faces.size(); ++i)
                if (value.contains(kast::kSkyFaceNames[i]) && value[kast::kSkyFaceNames[i]].is_string())
                    faces[i] = value[kast::kSkyFaceNames[i]].get<std::string>();
            skies.insert(faces);
        }
        sceneTextures(value, usages, skies);
    }
}

// Compresses every image among paths (game.pak's PNG / JPG files, and the images embedded in its models) that a
// material, model, particle, .kshader / graph param or sky can sample into KTX entries at kast::texturePakPath, through
// the editor's texture cache, one per usage it is sampled with (materialTextures; a scene's or prefab's particles: Color);
// an image nothing names (Lua may use it) as Color. Each sky's six faces become one cube at kast::skyPakPath. Takes an
// image out of paths unless the GUI (an .rml / .rcss / .guiprefab) or Lua names it, or nothing does, or it is a terrain
// heightmap (maps/<map>/heights/) or a face of a sky that can't be cooked: RmlUi, terrain and the player's sky fallback
// read source images. False + error naming the first image that fails.
// ponytail: an image counts as named by .rml / .rcss / .guiprefab / .lua text when its file name appears in it
// (over-inclusive: a same-named image elsewhere ships its source too); parse their paths if pak size matters.
bool cookShippedTextures(const std::filesystem::path& root, std::vector<std::string>& paths,
                         const std::map<std::string, kast::ShaderSource>& sources, TextureUsages usages,
                         const std::map<std::string, std::string>& embedded, std::map<std::string, std::string>& cooked,
                         std::string& error)
{
    std::set<std::array<std::string, 6>> skies;
    std::string guiText, luaText;
    for (const std::string& path : paths) {
        const std::filesystem::path file = root / fromUtf8(path);
        std::string text;
        if (path.ends_with(".mat")) {
            kast::Material material;
            if (std::string ignored; kast::loadMaterial(file, material, ignored)) {
                const auto shader = sources.find(material.shader);
                for (const auto& [texture, usage] :
                     kast::materialTextures(material, shader == sources.end() ? nullptr : &shader->second))
                    usages[texture].insert(usage);
            }
        } else if ((path.ends_with(".scene") || path.ends_with(".prefab")) && kast::readFile(file, text)) {
            sceneTextures(Json::parse(text, nullptr, false), usages, skies);
        } else if ((path.ends_with(".rml") || path.ends_with(".rcss") || path.ends_with(kast::kGuiPrefabExtension)) &&
                   kast::readFile(file, text)) {
            guiText += text;
        } else if (path.ends_with(".lua") && kast::readFile(file, text)) {
            luaText += text;
        }
    }

    const std::string version = texturecVersion();
    const std::filesystem::path cacheDir = root / kEditorDir / "texture_cache";
    const auto cook = [&](const std::string& path, const std::string& bytes, kast::TextureUsage usage) {
        std::string ktx, reason;
        bool cached = false;
        if (kast::isGpuImage(bytes)) { // an embedded KTX / DDS ships as stored
            ktx = bytes;
        } else if (!kast::cookTexture(bytes, usage, cookTextureWithTexturec, version, cacheDir, ktx, cached, reason)) {
            error = std::format("Build failed: texture '{}': {}", path, reason);
            return false;
        }
        cooked[kast::texturePakPath(path, usage)] = std::move(ktx);
        kast::log::info(std::format("Cooked texture '{}' as {} for game.pak{}", path, kast::usageName(usage),
                                    cached ? " (cache hit)" : ""));
        return true;
    };
    // Each sky's six faces become one cube. A sky whose faces don't make one (one missing, unreadable, or a different
    // size) gets none, and its faces ship as files: the player reads them to say why, and draws the procedural sky, as
    // the editor does.
    std::set<std::string> skyFaces, brokenSkyFaces;
    for (const auto& faces : skies) {
        std::array<kast::MipImage, 6> images;
        bool decoded = true;
        for (size_t i = 0; i < faces.size() && decoded; ++i) {
            std::string bytes, ignored;
            const std::optional<std::filesystem::path> file = kast::resolveProjectPath(root, faces[i], ignored);
            std::optional<kast::MipImage> image;
            decoded = file && kast::readFile(*file, bytes) && (image = kast::decodeImage(bytes, true));
            if (decoded)
                images[i] = std::move(*image);
        }
        if (!decoded || !kast::skyFacesError(images).empty()) {
            brokenSkyFaces.insert(faces.begin(), faces.end());
            continue;
        }
        std::string ktx, reason;
        bool cached = false;
        if (!kast::cookTexture(kast::skyCubeKtx(images), kast::TextureUsage::Color, cookTextureWithTexturec, version,
                               cacheDir, ktx, cached, reason)) {
            error = std::format("Build failed: sky cube of '{}': {}", faces[0], reason);
            return false;
        }
        cooked[kast::skyPakPath(faces)] = std::move(ktx);
        skyFaces.insert(faces.begin(), faces.end());
        kast::log::info(std::format("Cooked sky cube of '{}' for game.pak{}", faces[0], cached ? " (cache hit)" : ""));
    }
    std::set<std::string> cookedOnly;
    for (const std::string& path : paths) {
        if (!isImageFile(path))
            continue;
        const std::string name = path.substr(path.rfind('/') + 1);
        const bool heightmap = std::filesystem::path(fromUtf8(path)).parent_path().filename() == "heights";
        const bool gui = heightmap || brokenSkyFaces.contains(path) || guiText.find(name) != std::string::npos;
        const bool lua = luaText.find(name) != std::string::npos;
        const auto used = usages.find(path);
        const bool named = used != usages.end() || skyFaces.contains(path);
        std::set<kast::TextureUsage> cookAs = used == usages.end() ? std::set<kast::TextureUsage>{} : used->second;
        if (!named && !(gui && !lua))
            cookAs.insert(kast::TextureUsage::Color);
        std::string bytes;
        if (!cookAs.empty() && !kast::readFile(root / fromUtf8(path), bytes)) {
            error = std::format("Build failed: texture '{}': cannot read the file", path);
            return false;
        }
        for (const kast::TextureUsage usage : cookAs)
            if (!cook(path, bytes, usage))
                return false;
        if (named && !gui && !lua)
            cookedOnly.insert(path);
    }
    for (const auto& [path, bytes] : embedded)
        if (const auto used = usages.find(path); used != usages.end())
            for (const kast::TextureUsage usage : used->second)
                if (!cook(path, bytes, usage))
                    return false;
    // ORM pairs (materialTextures): their two images packed into one (kast::ormImage), cooked as it. A pair with an image
    // missing is skipped, as a missing texture is: the player reports it.
    for (const auto& [path, used] : usages) {
        const auto pair = kast::ormSources(path);
        if (!pair)
            continue;
        std::string images[2], reason;
        bool read = true;
        for (size_t i = 0; i < 2 && read; ++i) {
            const std::string& image = i == 0 ? pair->first : pair->second;
            if (const auto it = embedded.find(image); it != embedded.end())
                images[i] = it->second;
            else
                read = kast::readFile(root / fromUtf8(image), images[i]);
        }
        if (!read)
            continue;
        const std::optional<std::string> packed = kast::ormImage(images[0], images[1], reason);
        if (!packed) {
            error = std::format("Build failed: texture '{}': {}", path, reason);
            return false;
        }
        if (!cook(path, *packed, kast::TextureUsage::Linear))
            return false;
    }
    std::erase_if(paths, [&](const std::string& path) { return cookedOnly.contains(path); });
    return true;
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

bool checkOnlineConfig(const kast::Project& project, const kast::OnlineConfig& online, std::string& error)
{
    kast::Project candidate = project;
    candidate.online = online;
    std::string reason = kast::onlineConfigError(project.root, online), caPem;
    if (reason.empty() && kast::loadOnlineCa(candidate, caPem, reason) && online.enabled && online.caFile &&
        !shipsInPak(*online.caFile))
        reason = std::format("CA file '{}' is a dotfile or in a dot-folder, which game.pak leaves out", *online.caFile);
    if (reason.empty())
        return true;
    error = "Online Services: " + reason;
    return false;
}

bool setOnlineConfig(kast::Project& project, const kast::OnlineConfig& online, std::string& error)
{
    if (!checkOnlineConfig(project, online, error))
        return false;
    kast::Project updated = project;
    updated.online = online;
    if (!kast::saveProject(updated, error))
        return false;
    project = std::move(updated);
    kast::log::info(std::format("Online Services {} in {}", online.enabled ? "enabled" : "disabled", kast::kProjectFileName));
    return true;
}

bool setGuiConfig(kast::Project& project, const kast::GuiConfig& gui, std::string& error)
{
    kast::GuiConfig checked; // the rules project.kast loads by
    if (std::string reason; !kast::guiConfigFromJson(kast::guiConfigToJson(gui), checked, reason)) {
        error = "Display & GUI Scaling: " + reason;
        return false;
    }
    kast::Project updated = project;
    updated.gui = gui;
    if (!kast::saveProject(updated, error))
        return false;
    project = std::move(updated);
    kast::log::info(std::format("Display & GUI Scaling saved in {}", kast::kProjectFileName));
    return true;
}

bool buildGame(const kast::Project& project, const std::filesystem::path& outDir, const std::filesystem::path& playerDir,
               std::filesystem::path& exe, std::string& error)
{
    if (!checkStartScene(project, error))
        return false;
    if (std::string reason; !checkOnlineConfig(project, project.online, reason)) {
        error = "Build failed: " + reason;
        return false;
    }
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

    std::vector<std::string> contents = pakContents(project.root, out);
    std::map<std::string, std::string> generated; // shader bytecode, cooked models and textures
    std::map<std::string, kast::ShaderSource> shaders;
    TextureUsages usages;
    std::map<std::string, std::string> embedded;
    if (!compileShipped(project.root, contents, generated, shaders, error) ||
        !cookShippedModels(project.root, contents, generated, usages, embedded, error) ||
        !cookShippedTextures(project.root, contents, shaders, usages, embedded, generated, error))
        return false;

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) {
        error = std::format("Build failed: cannot create '{}': {}", utf8(out), ec.message());
        return false;
    }
    if (!kast::writePak(project.root, contents, out / kGamePakName, error, generated)) {
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
