#include "gui_scale.h"

#include "files.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr int kMaxSize = 16384; // a window or reference side, in pixels
constexpr const char* kUserKeys[] = {"mode", "width", "height", "refresh", "uiScale"};

bool wholeIn(const Json& value, int64_t min, int64_t max)
{
    return value.is_number_integer() && value.get<int64_t>() >= min && value.get<int64_t>() <= max;
}

bool numberIn(const Json& value, double min, double max)
{
    return value.is_number() && value.get<double>() >= min && value.get<double>() <= max;
}

// Why one key's value is invalid (naming the key), or empty if it is valid.
std::string fieldError(const std::string& key, const Json& value)
{
    if (key == "mode")
        return value.is_string() && windowModeFromName(value.get<std::string>())
                   ? ""
                   : "mode must be \"windowed\", \"borderless\" or \"exclusive\"";
    if (key == "width" || key == "height")
        return wholeIn(value, 1, kMaxSize) ? "" : std::format("{} must be a whole number from 1 to {}", key, kMaxSize);
    if (key == "refresh")
        return wholeIn(value, 1, 1000) ? "" : "refresh must be a whole number of Hz from 1 to 1000";
    if (key == "uiScale")
        return numberIn(value, kMinUiScale, kMaxUiScale)
                   ? ""
                   : std::format("uiScale must be a number from {} to {}", kMinUiScale, kMaxUiScale);
    if (key == "referenceResolution")
        return value.is_array() && value.size() == 2 && wholeIn(value[0], 1, kMaxSize) && wholeIn(value[1], 1, kMaxSize)
                   ? ""
                   : std::format("referenceResolution must be [width, height], whole numbers from 1 to {}", kMaxSize);
    if (key == "match")
        return numberIn(value, 0.0, 1.0) ? "" : "match must be a number from 0 to 1";
    if (key == "useOsDpi")
        return value.is_boolean() ? "" : "useOsDpi must be true or false";
    return std::format("unknown key \"{}\"", key);
}

} // namespace

double guiReferenceFit(int width, int height, const std::optional<std::pair<int, int>>& reference, double match)
{
    if (!reference || reference->first <= 0 || reference->second <= 0 || width <= 0 || height <= 0)
        return 1.0;
    const double t = std::clamp(match, 0.0, 1.0);
    const double w = std::log2(static_cast<double>(width) / reference->first);
    const double h = std::log2(static_cast<double>(height) / reference->second);
    return std::exp2(w + (h - w) * t);
}

double guiDpRatio(const GuiScale& scale, int width, int height, double osScale)
{
    return guiReferenceFit(width, height, scale.referenceResolution, scale.match) * (scale.useOsDpi ? osScale : 1.0) *
           scale.uiScale;
}

const char* windowModeName(WindowMode mode)
{
    switch (mode) {
    case WindowMode::Borderless: return "borderless";
    case WindowMode::Exclusive: return "exclusive";
    default: return "windowed";
    }
}

std::optional<WindowMode> windowModeFromName(std::string_view name)
{
    for (const WindowMode mode : {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Exclusive})
        if (name == windowModeName(mode))
            return mode;
    return std::nullopt;
}

std::optional<DisplayModeInfo> pickExclusiveMode(const DisplayMode& want, const std::vector<DisplayModeInfo>& modes)
{
    std::optional<DisplayModeInfo> best;
    for (const DisplayModeInfo& mode : modes)
        if (mode.width == want.width && mode.height == want.height && (!want.refresh || mode.refresh == *want.refresh) &&
            (!best || mode.refresh > best->refresh))
            best = mode;
    return best;
}

bool guiConfigFromJson(const Json& json, GuiConfig& out, std::string& error)
{
    if (!json.is_object()) {
        error = "must be an object";
        return false;
    }
    GuiConfig config;
    for (const auto& [key, value] : json.items()) {
        if (std::string reason = fieldError(key, value); !reason.empty()) {
            error = std::move(reason);
            return false;
        }
        if (key == "referenceResolution")
            config.scale.referenceResolution = std::pair{value[0].get<int>(), value[1].get<int>()};
        else if (key == "match")
            config.scale.match = value.get<double>();
        else if (key == "useOsDpi")
            config.scale.useOsDpi = value.get<bool>();
        else if (key == "uiScale")
            config.scale.uiScale = value.get<double>();
        else if (key == "mode")
            config.window.mode = *windowModeFromName(value.get<std::string>());
        else if (key == "width")
            config.window.width = value.get<int>();
        else if (key == "height")
            config.window.height = value.get<int>();
        else if (key == "refresh")
            config.window.refresh = value.get<int>();
    }
    out = config;
    return true;
}

Json guiConfigToJson(const GuiConfig& config)
{
    Json json = Json::object();
    if (config.scale.referenceResolution)
        json["referenceResolution"] = {config.scale.referenceResolution->first, config.scale.referenceResolution->second};
    json["match"] = config.scale.match;
    json["useOsDpi"] = config.scale.useOsDpi;
    json["uiScale"] = config.scale.uiScale;
    json["mode"] = windowModeName(config.window.mode);
    json["width"] = config.window.width;
    json["height"] = config.window.height;
    if (config.window.refresh)
        json["refresh"] = *config.window.refresh;
    return json;
}

bool userDisplaySettingsFromText(std::string_view text, UserDisplaySettings& out, std::string& error)
{
    Json json;
    try {
        json = Json::parse(text);
    } catch (const Json::exception& e) {
        error = e.what();
        return false;
    }
    if (!json.is_object()) {
        error = "must be a JSON object";
        return false;
    }
    UserDisplaySettings settings;
    for (const auto& [key, value] : json.items()) {
        std::string reason = std::ranges::find(kUserKeys, key) == std::end(kUserKeys) ? std::format("unknown key \"{}\"", key)
                                                                                      : fieldError(key, value);
        if (!reason.empty()) {
            error = std::move(reason);
            return false;
        }
        if (key == "mode")
            settings.mode = windowModeFromName(value.get<std::string>());
        else if (key == "width")
            settings.width = value.get<int>();
        else if (key == "height")
            settings.height = value.get<int>();
        else if (key == "refresh")
            settings.refresh = value.get<int>();
        else
            settings.uiScale = value.get<double>();
    }
    out = settings;
    return true;
}

std::string userDisplaySettingsToText(const UserDisplaySettings& settings)
{
    Json json = Json::object();
    if (settings.mode)
        json["mode"] = windowModeName(*settings.mode);
    if (settings.width)
        json["width"] = *settings.width;
    if (settings.height)
        json["height"] = *settings.height;
    if (settings.refresh)
        json["refresh"] = *settings.refresh;
    if (settings.uiScale)
        json["uiScale"] = *settings.uiScale;
    return json.dump(2) + '\n';
}

UserDisplaySettings loadUserDisplaySettings(const std::filesystem::path& file)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return {};
    std::ifstream in(file, std::ios::binary); // never in the pak: kast::readFile would look only there
    std::stringstream text;
    text << in.rdbuf();
    UserDisplaySettings settings;
    if (std::string error; !in || !userDisplaySettingsFromText(text.str(), settings, error)) {
        log::warn(std::format("User settings '{}' are invalid ({}): using the project's defaults", file.string(),
                              error.empty() ? "cannot read the file" : error));
        return {};
    }
    return settings;
}

bool saveUserDisplaySettings(const std::filesystem::path& file, const UserDisplaySettings& settings)
{
    if (writeFileAtomic(file, userDisplaySettingsToText(settings)))
        return true;
    log::warn(std::format("Cannot write user settings '{}'", file.string()));
    return false;
}

DisplayMode applyUserDisplay(const DisplayMode& project, const UserDisplaySettings& user)
{
    DisplayMode mode = project;
    mode.mode = user.mode.value_or(mode.mode);
    mode.width = user.width.value_or(mode.width);
    mode.height = user.height.value_or(mode.height);
    if (user.refresh)
        mode.refresh = user.refresh;
    return mode;
}

std::string prefFolderName(std::string_view projectName)
{
    std::string out;
    for (const char c : projectName)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.' ? c : '_';
    const size_t first = out.find_first_not_of(" .");
    if (first == std::string::npos)
        return "Game";
    return out.substr(first, out.find_last_not_of(" .") - first + 1);
}

} // namespace kast
