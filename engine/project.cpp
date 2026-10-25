#include "project.h"
#include "asset_ref.h"

#include "files.h"
#include "log.h"
#include "network.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kProjectKeys[] = {"version", "name", "startScene"}; // required
constexpr const char* kOptionalProjectKeys[] = {"stateMachines", "audio", "online", "gui"};

std::string validate(const Json& json)
{
    if (!json.is_object())
        return "must be a JSON object";
    for (const char* key : kProjectKeys)
        if (!json.contains(key))
            return std::format("missing \"{}\"", key);
    for (const auto& [key, value] : json.items())
        if (std::ranges::find(kProjectKeys, key) == std::end(kProjectKeys) &&
            std::ranges::find(kOptionalProjectKeys, key) == std::end(kOptionalProjectKeys))
            return std::format("unknown key \"{}\"", key);
    if (!json["version"].is_number_integer() || json["version"].get<int>() != kProjectFileVersion)
        return std::format("unsupported \"version\" {} (expected {})", json["version"].dump(), kProjectFileVersion);
    if (!json["name"].is_string() || json["name"].get<std::string>().empty())
        return "\"name\" must be a non-empty string";
    if (!json["startScene"].is_string() && !json["startScene"].is_null())
        return "\"startScene\" must be a string or null";
    if (json.contains("stateMachines")) {
        const Json& machines = json["stateMachines"];
        if (!machines.is_array())
            return "\"stateMachines\" must be an array of state machine names";
        for (size_t i = 0; i < machines.size(); ++i) {
            if (!machines[i].is_string() || machines[i].get<std::string>().empty())
                return "\"stateMachines\" must hold non-empty strings";
            for (size_t j = 0; j < i; ++j)
                if (machines[j] == machines[i])
                    return std::format("\"stateMachines\" lists {} twice", machines[i].dump());
        }
    }
    if (json.contains("audio")) {
        // {"<bus>": {"volume": 0..1, "muted": bool}, ...}; every key optional
        const Json& audio = json["audio"];
        if (!audio.is_object())
            return "\"audio\" must be an object of buses";
        for (const auto& [bus, level] : audio.items()) {
            if (!busFromName(bus))
                return std::format("\"audio\": unknown bus \"{}\"", bus);
            if (!level.is_object())
                return std::format("\"audio\".{} must be an object", bus);
            for (const auto& [key, value] : level.items()) {
                if (key == "volume" && !(value.is_number() && value.get<double>() >= 0.0 && value.get<double>() <= 1.0))
                    return std::format("\"audio\".{}.volume must be a number between 0 and 1", bus);
                if (key == "muted" && !value.is_boolean())
                    return std::format("\"audio\".{}.muted must be true or false", bus);
                if (key != "volume" && key != "muted")
                    return std::format("\"audio\".{}: unknown key \"{}\"", bus, key);
            }
        }
    }
    if (json.contains("online")) {
        // {"enabled": bool, "authUrl": "...", "caFile": "..."}; every key optional, values checked by onlineConfigError
        const Json& online = json["online"];
        if (!online.is_object())
            return "\"online\" must be an object";
        for (const auto& [key, value] : online.items()) {
            if (key == "enabled" && !value.is_boolean())
                return "\"online\".enabled must be true or false";
            if ((key == "authUrl" || key == "caFile") && !value.is_string())
                return std::format("\"online\".{} must be a string", key);
            if (key != "enabled" && key != "authUrl" && key != "caFile")
                return std::format("\"online\": unknown key \"{}\"", key);
        }
    }
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
    std::string text;
    if (!readFile(file, text)) {
        error = std::format("Cannot open project '{}'", file.string());
        return false;
    }
    std::string reason;
    try {
        Json json = Json::parse(text);
        readAssetRefs(json);
        reason = validate(json);
        if (reason.empty()) {
            Project loaded{.root = normalized(file).parent_path(), .name = json["name"].get<std::string>()};
            if (json["startScene"].is_string())
                loaded.startScene = json["startScene"].get<std::string>();
            if (json.contains("stateMachines"))
                loaded.stateMachines = json["stateMachines"].get<std::vector<std::string>>();
            if (json.contains("audio"))
                for (const auto& [bus, level] : json["audio"].items()) {
                    BusMix& mix = loaded.audio[static_cast<int>(*busFromName(bus))];
                    mix.volume = level.value("volume", mix.volume);
                    mix.muted = level.value("muted", mix.muted);
                }
            if (json.contains("online")) {
                const Json& online = json["online"];
                loaded.online.enabled = online.value("enabled", false);
                loaded.online.authUrl = online.value("authUrl", "");
                if (online.contains("caFile"))
                    loaded.online.caFile = online["caFile"].get<std::string>();
                if (const std::string onlineError = onlineConfigError(loaded.root, loaded.online); !onlineError.empty())
                    reason = "\"online\": " + onlineError;
            }
            if (std::string guiError; reason.empty() && json.contains("gui") && !guiConfigFromJson(json["gui"], loaded.gui, guiError))
                reason = "\"gui\": " + guiError;
            if (reason.empty()) {
                out = std::move(loaded);
                return true;
            }
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
    if (project.stateMachines)
        json["stateMachines"] = *project.stateMachines;
    if (project.audio != AudioMix{}) {
        Json& audio = json["audio"];
        for (int i = 0; i < kAudioBusCount; ++i)
            audio[busName(static_cast<AudioBus>(i))] = {{"volume", project.audio[i].volume}, {"muted", project.audio[i].muted}};
    }
    if (project.online != OnlineConfig{}) {
        Json& online = json["online"];
        online["enabled"] = project.online.enabled;
        online["authUrl"] = project.online.authUrl;
        if (project.online.caFile)
            online["caFile"] = *project.online.caFile;
    }
    if (project.gui != GuiConfig{})
        json["gui"] = guiConfigToJson(project.gui);
    const std::filesystem::path file = project.root / kProjectFileName;
    writeAssetRefs(json);
    if (!writeFileAtomic(file, json.dump(2) + '\n')) {
        error = std::format("Cannot write project '{}'", file.string());
        return false;
    }
    return true;
}

std::string onlineConfigError(const std::filesystem::path& root, const OnlineConfig& online)
{
    std::string error;
    if (online.enabled && online.authUrl.empty())
        return "Auth URL is required when Online Services are enabled";
    if (!online.authUrl.empty() && !parseNetUrl(online.authUrl, {"http", "https"}, error))
        return std::format("Auth URL: {}", error);
    if (online.caFile && !resolveProjectPath(root, *online.caFile, error))
        return std::format("CA file: {}", error);
    return {};
}

bool loadOnlineCa(const Project& project, std::string& caPem, std::string& error)
{
    caPem.clear();
    if (!project.online.enabled || !project.online.caFile)
        return true;
    const std::string& path = *project.online.caFile;
    const std::optional<std::filesystem::path> file = resolveProjectPath(project.root, path, error);
    std::string pem;
    if (!file) {
        error = std::format("CA file: {}", error);
    } else if (!readFile(*file, pem)) {
        error = std::format("CA file '{}' is missing or can't be read", path);
    } else if (!isValidCaPem(pem)) {
        error = std::format("CA file '{}' holds no valid PEM certificate", path);
    } else {
        caPem = std::move(pem);
        return true;
    }
    return false;
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
