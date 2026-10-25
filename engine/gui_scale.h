#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Game GUI resolution and DPI scaling, and the player's display modes (docs/engine/game-gui.html#scaling): plain data and
// pure math, no SDL or RmlUi, so the hosts (kast_player, the editor's Game view and GUI Builder) and tests share them.
namespace kast {

// How a UIDocument scales by the dp ratio. Dp: only dp units (RmlUi's density-independent pixels) scale, px stay
// physical pixels. All: the document lays out at the window's size / dp ratio and draws scaled up, so px scale too.
enum class GuiScaling { Dp, All };

constexpr double kMinUiScale = 0.5; // the player's UI scale multiplier is clamped to this range
constexpr double kMaxUiScale = 3.0;

// What the dp ratio is made of: project.kast "gui" defaults, changed at run time by kast.gui.*.
struct GuiScale {
    std::optional<std::pair<int, int>> referenceResolution; // the size the GUI was designed for; unset: no fit
    double match = 0.5;   // 0 fits the width, 1 the height, between: a log-space blend of the two
    bool useOsDpi = true; // times the OS display scale (Windows' 125%, ...)
    double uiScale = 1.0; // the player's multiplier (kast.gui.setScale), kMinUiScale..kMaxUiScale

    bool operator==(const GuiScale&) const = default;
};

// Unity CanvasScaler's "Scale With Screen Size / Match Width Or Height": exp2(lerp(log2(w / refW), log2(h / refH), match)).
// 1 without a reference (or a non-positive size).
double guiReferenceFit(int width, int height, const std::optional<std::pair<int, int>>& reference, double match);
// dpRatio = refFit x (useOsDpi ? osScale : 1) x uiScale, for a GUI of width x height pixels (> 0).
double guiDpRatio(const GuiScale& scale, int width, int height, double osScale);

enum class WindowMode { Windowed, Borderless, Exclusive };
const char* windowModeName(WindowMode mode); // "windowed", "borderless", "exclusive"
std::optional<WindowMode> windowModeFromName(std::string_view name);

// A window mode. Windowed: a window of width x height; Borderless: fullscreen at the desktop's size (width x height are
// the windowed size to go back to); Exclusive: the display switches to width x height at refresh Hz (unset: the
// highest that size has).
struct DisplayMode {
    WindowMode mode = WindowMode::Windowed;
    int width = 1280;
    int height = 720;
    std::optional<int> refresh;

    bool operator==(const DisplayMode&) const = default;
};

// One of a display's exclusive fullscreen modes (refresh rounded to whole Hz).
struct DisplayModeInfo {
    int width = 0;
    int height = 0;
    int refresh = 0;

    bool operator==(const DisplayModeInfo&) const = default;
};

// The exclusive mode for want among a display's modes: its size at want.refresh, or at the highest refresh if unset.
// nullopt if the display has none.
std::optional<DisplayModeInfo> pickExclusiveMode(const DisplayMode& want, const std::vector<DisplayModeInfo>& modes);

// project.kast "gui": the scale defaults and the player's default window. All defaults = the block is left out.
struct GuiConfig {
    GuiScale scale;
    DisplayMode window;

    bool operator==(const GuiConfig&) const = default;
};

// The "gui" block, strict: a wrong type or value is an error naming the field ("match must be a number between 0 and
// 1"). Missing keys keep their defaults.
bool guiConfigFromJson(const nlohmann::ordered_json& json, GuiConfig& out, std::string& error);
nlohmann::ordered_json guiConfigToJson(const GuiConfig& config); // every key; unset optional ones left out

// What a player chose (kast.display.setMode, kast.gui.setScale), kept per user over the project's defaults. Every
// field optional: unset = the project's.
struct UserDisplaySettings {
    std::optional<WindowMode> mode;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> refresh;
    std::optional<double> uiScale;

    bool operator==(const UserDisplaySettings&) const = default;
};

// The settings file's JSON ({"mode", "width", "height", "refresh", "uiScale"}, each optional), as strict as "gui".
bool userDisplaySettingsFromText(std::string_view text, UserDisplaySettings& out, std::string& error);
std::string userDisplaySettingsToText(const UserDisplaySettings& settings);
// Reads file: a missing one gives empty settings; a corrupt or invalid one logs a warning and gives empty settings too
// (the project's defaults apply).
UserDisplaySettings loadUserDisplaySettings(const std::filesystem::path& file);
bool saveUserDisplaySettings(const std::filesystem::path& file, const UserDisplaySettings& settings); // false: logged
// The project's window with the user's choices over it.
DisplayMode applyUserDisplay(const DisplayMode& project, const UserDisplaySettings& user);
// A project name as a folder name (SDL_GetPrefPath's app): letters, digits, ' ', '-', '_' and '.' kept, anything else
// '_', trimmed; "Game" if nothing is left.
std::string prefFolderName(std::string_view projectName);

} // namespace kast
