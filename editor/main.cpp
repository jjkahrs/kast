#include "build.h"
#include "document.h"
#include "editor_settings.h"
#include "external_editor.h"
#include "game_view.h"
#include "imgui_bgfx.h"
#include "launcher.h"
#include "online.h"
#include "online_panel.h"
#include "panels.h"
#include "recent_projects.h"
#include "ui_common.h"
#include "viewport.h"

#include <app.h>
#include <audio.h>
#include <crash_dialogs.h>
#include <gui.h>
#include <log.h>
#include <world_client.h>

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <ImGuizmo.h> // after imgui.h, which it needs
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h> // DockBuilder API
#include <imgui_stdlib.h>

#include <cassert>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Each viewport's shadow maps, then its scene and post passes (see SceneViewport, GameViewport).
constexpr bgfx::ViewId kViewportViews = kast::Renderer::kShadowViews + kast::PostProcess::kViews;
constexpr bgfx::ViewId kSceneView = 1; // then its overlays
constexpr bgfx::ViewId kGameView = kSceneView + kViewportViews + 1;
constexpr bgfx::ViewId kGameGuiView = kGameView + kViewportViews; // the game GUIs, over the Game view's scene
constexpr bgfx::ViewId kGuiPreviewView = kGameGuiView + 1; // the GUI Builder's preview
constexpr bgfx::ViewId kImGuiView = 255; // last, so the UI draws over everything
constexpr uint32_t kBackbufferClearColor = 0x202020ff;
// --add-component-popup, --open-menu, --open-build-settings, --open-online-settings, --open-preferences, --focus-panel and --assets-* wait this long: focus changes while the first frames dock the windows close
// popups.
constexpr int kAddComponentPopupFrame = 10;
constexpr float kComponentPollSeconds = 0.5f; // Lua definition files and the Assets tree are checked this often
constexpr const char* kUsage =
    "Usage: kast_editor [--frames N] [--screenshot out.png] [--screenshot-on-log frames,text] [--size WxH] [--window-pos X,Y] [--ui-scale F] [--projects-file list.json] "
    "[--scene in.scene] [--project dir] [--save-scene out.scene] [--select ID] [--play] [--select-network-entity] [--no-scene-post] [--add-component-popup] "
    "[--new-component-prompt Name] [--new-component Name] [--new-state-machine-prompt Name] [--build dir] "
    "[--open-menu Name] [--open-build-settings] [--open-online-settings] [--assets-menu path|/] [--assets-create system|fsm|gui|material:folder] [--assets-select path] "
    "[--focus-panel Window] [--open-gui file.rml] [--gui-select id] [--gui-preview-size WxH] [--gui-pick X,Y] "
    "[--gui-add Panel|Text|Button] [--gui-save] [--gui-click id] [--sim-mouse frame,none|left|right[+shift],dx,dy,wheel[,sx,sy]] [--sim-key frame,key]... [--sim-on-log text] [--settings-file settings.json] [--open-preferences] "
    "[--open-external path] [--drop-model path] [--world-address host:port] [--online-login-env] [--online-compose up|down|ps] [--log-fps] [--no-vsync] [--no-audio-device] [--crash abort|assert] | "
    "[--launcher] [--new-project name dir] [--launcher-open dir] [-- editor flags...]";

// --sim-mouse: a scripted mouse drag and/or wheel turn on the Game panel, for screenshots of mouse-driven games.
struct SimMouse {
    int frame = 0;  // the press (at the Game image's center) and the wheel go out on this frame
    int button = 0; // SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT, 0 = none
    float dx = 0.0f; // moved the frame after the press; released the frame after that
    float dy = 0.0f;
    float wheel = 0.0f; // notches, + = away from the user
    float sx = 0.0f;    // where the press goes, from the Game image's center (",sx,sy" after wheel)
    float sy = 0.0f;
    bool shift = false; // "left+shift": Shift reads as held from the press until after the release
};

// --sim-key: one key press on the Game panel (down, its typed character, up two frames later), for tests of game keys.
struct SimKey {
    int frame = 0;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    SDL_Keycode key = SDLK_UNKNOWN;
    std::string text; // what typing it inserts into a focused text field ("b"); empty for keys that type nothing
};

struct Options {
    int frames = 0;         // 0 = run until quit
    std::string screenshot; // captured after the last frame
    std::optional<std::pair<int, std::string>> screenshotOnLog; // --screenshot-on-log: delay frames, log text
    std::optional<std::pair<int, int>> size; // window size; default 1280x720 (editor), 1024x600 (Project Manager)
    std::optional<std::pair<int, int>> windowPos; // window top-left (editor and Project Manager)
    std::optional<float> uiScale; // overrides the monitor's display scale
    std::string project;          // project directory opened at startup (before --scene)
    std::string scene;            // opened at startup instead of the default scene
    std::string saveScene;        // save the startup scene here and exit, without opening a window
    kast::EntityId select = 0;    // select this entity at startup; 0 = none
    bool play = false;            // enter Play mode once the startup scene is loaded
    bool selectNetworkEntity = false; // with --play: select the first replicated clone (NetworkEntity) once it exists
    bool noScenePost = false;     // start with the Scene view's "Post" toggle off
    std::optional<std::pair<std::string, std::string>> newProject; // name, dir: open the prefilled New Project modal
    bool addComponentPopup = false; // open the Inspector's Add Component popup for the --select entity
    std::optional<std::string> newComponentPrompt; // open the Inspector's New Lua Component modal with this name
    std::string newComponent; // create this Lua component and add it to the --select entity at startup
    std::optional<std::string> newStateMachinePrompt; // open the State Machines panel's New State Machine modal
    std::string build;        // build the --project game into this folder and exit, without opening a window
    std::string openMenu;     // open this main menu (e.g. "Build") for screenshots
    std::optional<std::string> assetsMenu; // open this Assets entry's context menu + Create submenu ("" = background)
    std::string assetsSelect; // select this Assets entry (project-relative) as a click does: a .mat shows in the Inspector
    std::optional<std::pair<Panels::AssetCreate, std::string>> assetsCreate; // run Assets Create in that folder
    bool openBuildSettings = false; // open the Build Settings modal (needs --project) for screenshots
    bool openOnlineSettings = false; // open the Online Services modal (needs --project) for screenshots
    std::string focusPanel;         // bring this window (e.g. "Events"; "Lighting", "Post Processing", "Physics", "Mixer", "Network" and "Online" also open it) to the front, for screenshots
    std::string openGui;            // open this .rml in the GUI Builder (relative paths: to the project, else the cwd)
    std::string guiSelect;          // then select the element with this id in it
    std::optional<std::pair<int, int>> guiPreviewSize; // add this GUI Preview resolution and pick it
    std::optional<std::pair<int, int>> guiPick; // click the GUI Preview here (preview pixels, kAddComponentPopupFrame)
    std::optional<GuiKind> guiAdd; // then add this element as the GUI Hierarchy's + buttons do
    bool guiSave = false;          // then save the GUI as its Save button does
    std::string guiClick;           // with --play: click the game GUI element with this id (frame kAddComponentPopupFrame)
    std::optional<SimMouse> simMouse; // with --play: push these SDL mouse events (the cursor lock isn't applied)
    std::vector<SimKey> simKeys;      // with --play: these key presses
    std::string simOnLog;             // --sim-on-log: simMouse's and simKeys' frames count from the first log line with this
    std::string projectsFile;       // the recent-projects list; empty = the one in %APPDATA%
    std::string settingsFile;       // the editor settings (editor_settings.h); empty = the one in %APPDATA%
    bool openPreferences = false;   // open the File > Preferences modal for screenshots
    std::string openExternal;       // open this project file (path relative to the project) in the external editor
    bool launcher = false;          // run the Project Manager (the default anyway when nothing asks for the editor)
    std::string launcherOpen;       // open this project as the Project Manager would, without a window, and exit
    std::string dropModel;          // add this project model file as dropping it from the Assets panel does
    std::string worldAddress;       // host:port replacing every realm's world address in Play (kast.world), editor only
    bool onlineLoginEnv = false;    // Online panel test login from KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD (--play waits for it)
    std::optional<OnlineModel::Compose> onlineCompose; // press the Online panel's Start, Stop or Status at startup
    bool logFps = false;            // log frames per second, editor CPU and GPU frame times once a second
    std::string crash;              // --crash abort|assert: fail on purpose, to test KAST_NO_CRASH_DIALOGS
    bool noVsync = false;           // present as fast as possible, to measure the frame rate past the display's
    bool noAudioDevice = false;     // play sound on the null backend (silent), as if there were no output device
    std::vector<std::string> childArgs; // the flags after --, passed on to the editor the Project Manager starts

    // The editor runs with a project or scene to edit (or to save or build from); otherwise the Project Manager does.
    bool editor() const { return !project.empty() || !scene.empty() || !saveScene.empty() || !build.empty(); }
};

template <typename T>
bool parsePositive(std::string_view text, T& out)
{
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end && out > 0;
}

// --sim-mouse's events for this frame, handled from the next one: the cursor moves to the Game image's center and
// (offset by sx, sy) presses (and turns the wheel), then moves by dx, dy, then releases there (so a GUI sees a drag
// and drop).
void pushSimMouse(const SimMouse& sim, int frame, ImVec2 center, SDL_WindowID window)
{
    const bool moved = frame > sim.frame;
    const float x = center.x + sim.sx + (moved ? sim.dx : 0.0f);
    const float y = center.y + sim.sy + (moved ? sim.dy : 0.0f);
    const auto push = [&](SDL_EventType type, float xrel = 0.0f, float yrel = 0.0f) {
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion = {.type = type, .windowID = window, .x = x, .y = y, .xrel = xrel, .yrel = yrel};
        } else if (type == SDL_EVENT_MOUSE_WHEEL) {
            event.wheel = {.type = type, .windowID = window, .y = sim.wheel, .direction = SDL_MOUSEWHEEL_NORMAL,
                           .mouse_x = center.x, .mouse_y = center.y};
        } else {
            event.button = {.type = type, .windowID = window, .button = static_cast<Uint8>(sim.button),
                            .down = type == SDL_EVENT_MOUSE_BUTTON_DOWN, .clicks = 1, .x = x, .y = y};
        }
        SDL_PushEvent(&event);
    };
    if (frame == sim.frame) {
        push(SDL_EVENT_MOUSE_MOTION);
        if (sim.button)
            push(SDL_EVENT_MOUSE_BUTTON_DOWN);
        if (sim.wheel != 0.0f)
            push(SDL_EVENT_MOUSE_WHEEL);
    } else if (frame == sim.frame + 1 || (!sim.button && frame > sim.frame)) { // no button: a hover, held there
        push(SDL_EVENT_MOUSE_MOTION, frame == sim.frame + 1 ? sim.dx : 0.0f, frame == sim.frame + 1 ? sim.dy : 0.0f);
    } else if (sim.button && frame == sim.frame + 2) {
        push(SDL_EVENT_MOUSE_BUTTON_UP);
    }
    if (sim.shift && (frame == sim.frame || frame == sim.frame + 3))
        SDL_SetModState(frame == sim.frame ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE);
}

// "frame,none|left|right,dx,dy,wheel" with a positive frame; logs why not.
bool parseSimMouse(std::string_view text, std::optional<SimMouse>& out)
{
    std::vector<std::string_view> parts;
    for (size_t start = 0;;) {
        const size_t at = text.find(',', start);
        parts.push_back(text.substr(start, at - start));
        if (at == std::string_view::npos)
            break;
        start = at + 1;
    }
    const auto number = [](std::string_view part, float& value) {
        auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        return ec == std::errc{} && ptr == part.data() + part.size();
    };
    SimMouse sim;
    std::string_view button = parts.size() > 1 ? parts[1] : "";
    if (button.ends_with("+shift")) {
        sim.shift = true;
        button.remove_suffix(6);
    }
    const bool ok = (parts.size() == 5 || (parts.size() == 7 && number(parts[5], sim.sx) && number(parts[6], sim.sy))) &&
                    parsePositive(parts[0], sim.frame) && (button == "none" || button == "left" || button == "right") &&
                    number(parts[2], sim.dx) && number(parts[3], sim.dy) && number(parts[4], sim.wheel);
    if (!ok) {
        kast::log::error(
            std::format("Invalid --sim-mouse '{}', expected frame,none|left|right[+shift],dx,dy,wheel[,sx,sy]", text));
        return false;
    }
    sim.button = button == "left" ? SDL_BUTTON_LEFT : button == "right" ? SDL_BUTTON_RIGHT : 0;
    out = sim;
    return true;
}

// --sim-key's events for this frame: down and the typed text on its frame, up two frames later.
void pushSimKey(const SimKey& sim, int frame, SDL_WindowID window)
{
    if (frame != sim.frame && frame != sim.frame + 2)
        return;
    const bool down = frame == sim.frame;
    SDL_Event event{};
    event.key = {.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, .windowID = window, .scancode = sim.scancode,
                 .key = sim.key, .down = down};
    SDL_PushEvent(&event);
    if (down && !sim.text.empty()) {
        SDL_Event text{};
        text.text = {.type = SDL_EVENT_TEXT_INPUT, .windowID = window, .text = sim.text.c_str()};
        SDL_PushEvent(&text);
    }
}

// "frame,key" with a positive frame and an SDL key name ("B", "Escape"); logs why not.
bool parseSimKey(std::string_view text, std::vector<SimKey>& out)
{
    const size_t comma = text.find(',');
    SimKey sim;
    if (comma != std::string_view::npos && parsePositive(text.substr(0, comma), sim.frame))
        sim.scancode = SDL_GetScancodeFromName(std::string(text.substr(comma + 1)).c_str());
    if (sim.scancode == SDL_SCANCODE_UNKNOWN) {
        kast::log::error(std::format("Invalid --sim-key '{}', expected frame,key (an SDL key name such as B)", text));
        return false;
    }
    sim.key = SDL_GetKeyFromScancode(sim.scancode, SDL_KMOD_NONE, false);
    if (sim.key >= ' ' && sim.key < 0x7f)
        sim.text = std::string(1, static_cast<char>(sim.key));
    out.push_back(std::move(sim));
    return true;
}

// "<A><separator><B>" with positive A and B; logs why not.
bool parsePair(std::string_view text, char separator, std::optional<std::pair<int, int>>& out)
{
    const size_t at = text.find(separator);
    int a = 0;
    int b = 0;
    if (at == std::string_view::npos || !parsePositive(text.substr(0, at), a) || !parsePositive(text.substr(at + 1), b)) {
        kast::log::error(std::format("Invalid '{}', expected two positive numbers separated by '{}'", text, separator));
        return false;
    }
    out.emplace(a, b);
    return true;
}

bool parseSize(std::string_view size, std::optional<std::pair<int, int>>& out) { return parsePair(size, 'x', out); }

// "Panel", "Text" or "Button".
bool parseGuiKind(std::string_view label, std::optional<GuiKind>& out)
{
    for (GuiKind kind : kGuiCreatableKinds)
        if (label == guiKindLabel(kind)) {
            out = kind;
            return true;
        }
    return false;
}

// "system:<folder>", "fsm:<folder>", "gui:<folder>" or "material:<folder>".
bool parseAssetCreate(std::string_view value, std::optional<std::pair<Panels::AssetCreate, std::string>>& out)
{
    constexpr std::pair<std::string_view, Panels::AssetCreate> kinds[] = {{"system:", Panels::AssetCreate::System},
                                                                          {"fsm:", Panels::AssetCreate::StateMachine},
                                                                          {"gui:", Panels::AssetCreate::Gui},
                                                                          {"material:", Panels::AssetCreate::Material}};
    for (const auto& [prefix, kind] : kinds)
        if (value.starts_with(prefix)) {
            out.emplace(kind, std::string(value.substr(prefix.size())));
            return true;
        }
    return false;
}

std::optional<Options> parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--frames" && hasValue && parsePositive(argv[i + 1], options.frames)) {
            ++i;
        } else if (arg == "--screenshot" && hasValue) {
            options.screenshot = argv[++i];
        } else if (arg == "--screenshot-on-log" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t comma = text.find(',');
            int delay = 0;
            if (comma == std::string_view::npos || comma + 1 == text.size() || !parsePositive(text.substr(0, comma), delay)) {
                kast::log::error(std::format("Invalid --screenshot-on-log '{}', expected frames,text", text));
                return std::nullopt;
            }
            options.screenshotOnLog = {{delay, std::string(text.substr(comma + 1))}};
        } else if (arg == "--scene" && hasValue) {
            options.scene = argv[++i];
        } else if (arg == "--project" && hasValue) {
            options.project = argv[++i];
        } else if (arg == "--save-scene" && hasValue) {
            options.saveScene = argv[++i];
        } else if (arg == "--select" && hasValue && parsePositive(argv[i + 1], options.select)) {
            ++i;
        } else if (arg == "--new-project" && i + 2 < argc) {
            options.newProject.emplace(argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (arg == "--add-component-popup") {
            options.addComponentPopup = true;
        } else if (arg == "--new-component-prompt" && hasValue) {
            options.newComponentPrompt = argv[++i];
        } else if (arg == "--new-state-machine-prompt" && hasValue) {
            options.newStateMachinePrompt = argv[++i];
        } else if (arg == "--new-component" && hasValue) {
            options.newComponent = argv[++i];
        } else if (arg == "--build" && hasValue) {
            options.build = argv[++i];
        } else if (arg == "--assets-menu" && hasValue) {
            const std::string_view path = argv[++i];
            options.assetsMenu = std::string(path == "/" ? "" : path);
        } else if (arg == "--assets-select" && hasValue) {
            options.assetsSelect = argv[++i];
        } else if (arg == "--assets-create" && hasValue && parseAssetCreate(argv[i + 1], options.assetsCreate)) {
            ++i;
        } else if (arg == "--open-gui" && hasValue) {
            options.openGui = argv[++i];
        } else if (arg == "--gui-select" && hasValue) {
            options.guiSelect = argv[++i];
        } else if (arg == "--gui-click" && hasValue) {
            options.guiClick = argv[++i];
        } else if (arg == "--sim-on-log" && hasValue) {
            options.simOnLog = argv[++i];
        } else if (arg == "--sim-key" && hasValue) {
            if (!parseSimKey(argv[++i], options.simKeys))
                return std::nullopt;
        } else if (arg == "--sim-mouse" && hasValue) {
            if (!parseSimMouse(argv[++i], options.simMouse))
                return std::nullopt;
        } else if (arg == "--gui-add" && hasValue && parseGuiKind(argv[i + 1], options.guiAdd)) {
            ++i;
        } else if (arg == "--gui-save") {
            options.guiSave = true;
        } else if (arg == "--gui-pick" && hasValue) {
            if (!parsePair(argv[++i], ',', options.guiPick))
                return std::nullopt;
        } else if (arg == "--gui-preview-size" && hasValue) {
            if (!parseSize(argv[++i], options.guiPreviewSize))
                return std::nullopt;
        } else if (arg == "--open-menu" && hasValue) {
            options.openMenu = argv[++i];
        } else if (arg == "--focus-panel" && hasValue) {
            options.focusPanel = argv[++i];
        } else if (arg == "--projects-file" && hasValue) {
            options.projectsFile = argv[++i];
        } else if (arg == "--settings-file" && hasValue) {
            options.settingsFile = argv[++i];
        } else if (arg == "--select-network-entity") {
            options.selectNetworkEntity = true;
        } else if (arg == "--open-preferences") {
            options.openPreferences = true;
        } else if (arg == "--open-external" && hasValue) {
            options.openExternal = argv[++i];
        } else if (arg == "--drop-model" && hasValue) {
            options.dropModel = argv[++i];
        } else if (arg == "--log-fps") {
            options.logFps = true;
        } else if (arg == "--no-vsync") {
            options.noVsync = true;
        } else if (arg == "--no-audio-device") {
            options.noAudioDevice = true;
        } else if (arg == "--crash" && hasValue && (argv[i + 1] == std::string_view("abort") ||
                                                    argv[i + 1] == std::string_view("assert"))) {
            options.crash = argv[++i];
        } else if (arg == "--launcher") {
            options.launcher = true;
        } else if (arg == "--launcher-open" && hasValue) {
            options.launcherOpen = argv[++i];
        } else if (arg == "--") {
            options.childArgs.assign(argv + i + 1, argv + argc);
            break;
        } else if (arg == "--open-build-settings") {
            options.openBuildSettings = true;
        } else if (arg == "--open-online-settings") {
            options.openOnlineSettings = true;
        } else if (arg == "--play") {
            options.play = true;
        } else if (arg == "--world-address" && hasValue) {
            options.worldAddress = argv[++i];
            if (!kast::parseHostPort(options.worldAddress)) {
                kast::log::error(std::format("Invalid --world-address '{}', expected host:port. {}", options.worldAddress,
                                             kUsage));
                return std::nullopt;
            }
        } else if (arg == "--online-login-env") {
            options.onlineLoginEnv = true;
        } else if (arg == "--online-compose" && hasValue) {
            const std::string_view what = argv[++i];
            if (what != "up" && what != "down" && what != "ps") {
                kast::log::error(std::format("Invalid --online-compose '{}', expected up, down or ps. {}", what, kUsage));
                return std::nullopt;
            }
            options.onlineCompose = what == "up" ? OnlineModel::Compose::Up
                                    : what == "down" ? OnlineModel::Compose::Down : OnlineModel::Compose::Ps;
        } else if (arg == "--no-scene-post") {
            options.noScenePost = true;
        } else if (arg == "--size" && hasValue) {
            if (!parseSize(argv[++i], options.size))
                return std::nullopt;
        } else if (arg == "--window-pos" && hasValue) {
            const std::string_view value = argv[++i];
            options.windowPos = kast::parseWindowPos(value);
            if (!options.windowPos) {
                kast::log::error(std::format("Invalid --window-pos '{}', expected X,Y. {}", value, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--ui-scale" && hasValue) {
            const std::string_view value = argv[++i];
            float scale = 0.0f;
            if (!parsePositive(value, scale)) {
                kast::log::error(std::format("Invalid --ui-scale '{}', expected a positive number. {}", value, kUsage));
                return std::nullopt;
            }
            options.uiScale = clampUiScale(scale);
            if (*options.uiScale != scale)
                kast::log::warn(std::format("--ui-scale {} clamped to {:.2f}", value, *options.uiScale));
        } else {
            kast::log::error(std::format("Invalid argument '{}'. {}", arg, kUsage));
            return std::nullopt;
        }
    }
    const bool launcherFlags = options.launcher || options.newProject || !options.launcherOpen.empty() ||
                               !options.childArgs.empty();
    if (options.editor() && launcherFlags) {
        kast::log::error("--launcher, --new-project, --launcher-open and -- are Project Manager flags; they can't be "
                         "combined with --project, --scene, --save-scene or --build");
        return std::nullopt;
    }
    const bool editorFlags = options.select != 0 || options.play || options.selectNetworkEntity || options.addComponentPopup ||
                             options.newComponentPrompt || !options.newComponent.empty() || !options.openMenu.empty() ||
                             options.openBuildSettings || options.openOnlineSettings || !options.focusPanel.empty() || options.newStateMachinePrompt ||
                             options.assetsMenu || options.assetsCreate || !options.assetsSelect.empty() ||
                             !options.openGui.empty() ||
                             !options.guiSelect.empty() || options.guiPreviewSize || options.guiPick ||
                             options.guiAdd || options.guiSave || !options.guiClick.empty() || options.openPreferences ||
                             !options.openExternal.empty() || !options.dropModel.empty() || options.logFps ||
                             options.noVsync || !options.worldAddress.empty() || options.onlineLoginEnv ||
                             options.onlineCompose;
    if (!options.editor() && editorFlags) {
        kast::log::error("--select, --play, --select-network-entity, --add-component-popup, --new-component[-prompt], "
                         "--new-state-machine-prompt, --open-menu, --open-build-settings, --open-online-settings, --focus-panel, "
                         "--assets-menu, "
                         "--assets-create, --assets-select, --open-gui, --gui-select, --gui-preview-size, --gui-pick, --gui-add, --gui-save, "
                         "--gui-click, --open-preferences, --open-external, --world-address, --online-login-env and --online-compose require "
                         "--project <dir> or --scene <file>");
        return std::nullopt;
    }
    if (!options.saveScene.empty() && options.project.empty() && options.scene.empty()) {
        kast::log::error("--save-scene requires --scene <file> or --project <dir>");
        return std::nullopt;
    }
    if ((!options.guiSelect.empty() || options.guiPick || options.guiAdd || options.guiSave) && options.openGui.empty()) {
        kast::log::error("--gui-select, --gui-pick, --gui-add and --gui-save require --open-gui <file.rml>");
        return std::nullopt;
    }
    if (!options.guiClick.empty() && !options.play) {
        kast::log::error("--gui-click requires --play");
        return std::nullopt;
    }
    if (options.simMouse && !options.play) {
        kast::log::error("--sim-mouse requires --play");
        return std::nullopt;
    }
    if (!options.simKeys.empty() && !options.play) {
        kast::log::error("--sim-key requires --play");
        return std::nullopt;
    }
    if (!options.simOnLog.empty() && !options.simMouse && options.simKeys.empty()) {
        kast::log::error("--sim-on-log requires --sim-mouse or --sim-key");
        return std::nullopt;
    }
    if (!options.openExternal.empty() && options.project.empty()) {
        kast::log::error("--open-external requires --project <dir>");
        return std::nullopt;
    }
    if (options.openBuildSettings && options.project.empty()) {
        kast::log::error("--open-build-settings requires --project <dir>");
        return std::nullopt;
    }
    if (options.openOnlineSettings && options.project.empty()) {
        kast::log::error("--open-online-settings requires --project <dir>");
        return std::nullopt;
    }
    if ((!options.screenshot.empty() || options.screenshotOnLog) && options.frames == 0) {
        kast::log::error("--screenshot and --screenshot-on-log require --frames N");
        return std::nullopt;
    }
    return options;
}

void buildDefaultLayout(ImGuiID dockspace)
{
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.3f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, nullptr, &left);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    const ImGuiID leftLowest = ImGui::DockBuilderSplitNode(leftBottom, ImGuiDir_Down, 0.5f, nullptr, &leftBottom);
    ImGui::DockBuilderDockWindow("Systems", leftBottom);
    ImGui::DockBuilderDockWindow("Components", leftLowest);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Events", bottom);
    ImGui::DockBuilderDockWindow("State Machines", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Game", center);
    ImGui::DockBuilderDockWindow(GuiBuilder::kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(GuiBuilder::kInspectorWindow, right);
    ImGui::DockBuilderDockWindow(GuiBuilder::kStyleWindow, bottom);
    ImGui::DockBuilderDockWindow(GuiBuilder::kPreviewWindow, center);
    ImGui::DockBuilderFinish(dockspace);
}

// openMenu: a main menu to open this frame (--open-menu), or empty. openPreferences: set when File > Preferences... is
// chosen. showLighting, showPostProcessing, showPhysics, showMixer, showNetwork, showOnline: those panels' visibility,
// toggled in the Window menu.
void drawMenuBar(FileActions& files, Document& doc, const std::string& openMenu, bool& openPreferences,
                 bool& showLighting, bool& showPostProcessing, bool& showPhysics, bool& showMixer, bool& showNetwork,
                 bool& showOnline)
{
    if (ImGui::BeginMainMenuBar()) {
        if (!openMenu.empty())
            ImGui::OpenPopup(ImGui::GetID(openMenu.c_str())); // BeginMenu's popup id inside the menu bar
        if (ImGui::BeginMenu("File")) {
            const bool idle = files.idle();
            if (ImGui::MenuItem("New", "Ctrl+N", false, idle))
                files.newScene();
            if (ImGui::MenuItem("Open...", "Ctrl+O", false, idle))
                files.open();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, idle))
                files.save();
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, idle))
                files.saveAs();
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences..."))
                openPreferences = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                files.exit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("GameObject")) {
            for (ObjectKind kind : kObjectKinds)
                if (ImGui::MenuItem(objectKindLabel(kind)))
                    createObject(doc, kind, entt::null);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Build")) {
            const bool canBuild = files.canBuild();
            if (ImGui::MenuItem("Build Settings...", nullptr, false, canBuild))
                files.openBuildSettings();
            if (ImGui::MenuItem("Online Services...", nullptr, false, canBuild))
                files.openOnlineSettings();
            ImGui::Separator();
            if (ImGui::MenuItem("Build...", nullptr, false, canBuild))
                files.build();
            if (ImGui::MenuItem("Build and Run", nullptr, false, canBuild))
                files.buildAndRun();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            ImGui::MenuItem("Lighting", nullptr, &showLighting);
            ImGui::MenuItem("Post Processing", nullptr, &showPostProcessing);
            ImGui::MenuItem("Physics", nullptr, &showPhysics);
            ImGui::MenuItem("Mixer", nullptr, &showMixer);
            ImGui::MenuItem("Network", nullptr, &showNetwork);
            ImGui::MenuItem("Online", nullptr, &showOnline);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// File > Preferences...: the external editor command and the local services compose folder (per user, every project).
// open: open it this frame, showing the current settings.
void drawPreferences(ExternalEditor& editor, OnlineModel& online, std::string& draft, std::string& composeDraft, bool open)
{
    if (open) {
        draft = editor.command();
        composeDraft = online.composeFolder();
        ImGui::OpenPopup("Preferences");
    }
    if (!ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("External editor command");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 25.0f);
    ImGui::InputText("##externalEditor", &draft);
    ImGui::TextDisabled("{file} is replaced with the file's full path. Default: %s", kDefaultEditorCommand);
    ImGui::Spacing();
    ImGui::TextUnformatted("Local services compose folder");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 25.0f);
    ImGui::InputText("##composeFolder", &composeDraft);
    const std::string defaultFolder = defaultComposeFolder();
    ImGui::TextDisabled("Holds docker-compose.yml (Window > Online). Default: %s",
                        defaultFolder.empty() ? "none found" : defaultFolder.c_str());
    if (const std::string blocker = composeBlocker(true, composeDraft); !composeDraft.empty() && !blocker.empty())
        ImGui::TextColored(kErrorColor, "%s", blocker.c_str());
    if (ImGui::Button("Save")) {
        std::string error;
        if (editor.setCommand(draft, error) && online.setComposeFolder(composeDraft, error))
            ImGui::CloseCurrentPopup();
        else
            kast::log::error(error);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The GUI Builder's panels, each docked beside its scene counterpart in layouts saved before the builder existed:
// GUI Hierarchy by Hierarchy, GUI Inspector by Inspector, GUI Style by Console, GUI Preview by Scene.
void drawGuiBuilder(GuiBuilder& builder)
{
    const auto dockBeside = [](const char* existing, const char* window) {
        if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(existing));
            settings && !ImGui::FindWindowSettingsByID(ImHashStr(window)))
            ImGui::SetNextWindowDockID(settings->DockId, ImGuiCond_FirstUseEver);
    };
    dockBeside("Hierarchy", GuiBuilder::kHierarchyWindow);
    builder.drawHierarchy();
    dockBeside("Inspector", GuiBuilder::kInspectorWindow);
    builder.drawInspector();
    dockBeside("Console", GuiBuilder::kStyleWindow);
    builder.drawStyle();
    dockBeside("Scene", GuiBuilder::kPreviewWindow);
    builder.drawPreview();
    builder.drawModals();
}

void drawConsole()
{
    if (ImGui::Begin("Console")) {
        ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        // ponytail: draws every line each frame; switch to ImGuiListClipper when logs get long.
        for (const std::string& line : kast::log::lines())
            ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace

int main(int argc, char** argv)
{
    kast::disableCrashDialogsIfRequested();
    const std::optional<Options> options = parseArgs(argc, argv);
    if (!options)
        return 2;
    if (!options->crash.empty()) {
        kast::log::info(std::format("--crash {}", options->crash));
        assert(options->crash != "assert");
        std::abort(); // also ends --crash assert in a release build, where assert() is compiled out
    }
    const std::filesystem::path projectsFile =
        options->projectsFile.empty() ? defaultRecentFile() : std::filesystem::absolute(options->projectsFile);
    if (!options->editor()) {
        const LauncherOptions launcher{.projectsFile = projectsFile,
                                       .childArgs = options->childArgs,
                                       .frames = options->frames,
                                       .screenshot = options->screenshot,
                                       .width = options->size ? options->size->first : 1024,
                                       .height = options->size ? options->size->second : 600,
                                       .position = options->windowPos,
                                       .uiScale = options->uiScale,
                                       .newProject = options->newProject};
        return options->launcherOpen.empty() ? runLauncher(launcher) : launcherOpen(launcher, options->launcherOpen);
    }
    kast::log::info("Kast editor starting");
    if (options->noAudioDevice)
        kast::Audio::setDefaultOutput(kast::Audio::Output::Null);

    Document doc;
    std::optional<kast::Project> opened; // --project, recorded in the recent list once the editor window opens
    if (!options->project.empty()) {
        if (std::string error; !openProject(doc, std::filesystem::path(options->project) / kast::kProjectFileName, error)) {
            kast::log::error(error);
            return 1;
        }
        opened = doc.project;
    }
    if (!options->scene.empty()) {
        if (std::string error; !openFile(doc, options->scene, error)) { // its project walk-up wins over --project
            kast::log::error(error);
            return 1;
        }
    }
    if (!options->saveScene.empty()) {
        std::string error;
        if (!saveFile(doc, options->saveScene, error)) {
            kast::log::error(error);
            return 1;
        }
        return 0;
    }
    if (!options->build.empty()) {
        std::string error = "--build needs --project <dir>";
        std::filesystem::path exe;
        const char* base = SDL_GetBasePath(); // kast_player.exe is built next to the editor
        if (!options->project.empty() && doc.project && base &&
            buildGame(*doc.project, options->build, std::filesystem::path(reinterpret_cast<const char8_t*>(base)), exe, error))
            return 0;
        kast::log::error(base ? error : std::format("Cannot find the editor's folder: {}", SDL_GetError()));
        return 1;
    }
    if (options->select != 0 && !selectById(doc, options->select)) {
        kast::log::error(std::format("--select {}: no entity with that id", options->select));
        return 1;
    }
    if (!options->newComponent.empty()) { // before --play, which blocks it
        if (std::string error; !newLuaComponent(doc, doc.selected, options->newComponent, error))
            kast::log::error(std::format("--new-component {}: {}", options->newComponent, error));
    }
    if (!options->dropModel.empty())
        createModelEntity(doc, options->dropModel, entt::null);
    refreshStateMachines(doc); // the Inspector and State Machines panel list them from the first frame
    const std::filesystem::path settingsFile =
        options->settingsFile.empty() ? defaultEditorSettingsFile() : std::filesystem::absolute(options->settingsFile);
    // The Online panel's login goes to each Play session and comes back at Stop (refresh tokens rotate: one holder).
    OnlineModel online(settingsFile);
    doc.onPlayStarted = [&online](kast::LuaSystems& lua) { online.lendSession(lua.auth()); };
    doc.onPlayStopping = [&online](kast::LuaSystems& lua) { online.reclaimSession(lua.auth()); };
    // Editor Play's world address: --world-address, else the Online panel's override (applied every frame below).
    const auto applyWorldOverride = [&] {
        const std::string& address = options->worldAddress.empty() ? online.worldOverride() : options->worldAddress;
        const std::optional<std::string> wanted = address.empty() ? std::nullopt : std::optional(address);
        if (doc.lua.world().addressOverride() != wanted)
            doc.lua.world().setAddressOverride(wanted); // kept by every Play start
    };
    applyWorldOverride();
    GameViewport game(kGameView, kGameGuiView);
    doc.lua.setGui(&game.gui()); // kast.gui.* in Play, from the first (--play) start on
    bool playAfterLogin = false; // --play waiting for --online-login-env's answer
    if (options->onlineLoginEnv) {
        online.poll(doc.project ? &*doc.project : nullptr, false);
        const char* email = SDL_getenv("KAST_ONLINE_EMAIL");
        const char* password = SDL_getenv("KAST_ONLINE_PASSWORD");
        if (!email || !password) {
            kast::log::error("--online-login-env needs KAST_ONLINE_EMAIL and KAST_ONLINE_PASSWORD set");
        } else {
            std::string secret = password;
            online.login(email, secret); // wipes secret
            playAfterLogin = options->play;
        }
    }
    if (options->play && !playAfterLogin)
        startPlay(doc);
    if (options->onlineCompose && !online.runCompose(*options->onlineCompose))
        kast::log::error(std::format("--online-compose: {}", online.composeBlocker().empty() ? "could not start docker"
                                                                                            : online.composeBlocker()));
    if (std::string error; opened && !recordRecent(projectsFile, *opened, error))
        kast::log::warn(error);

    FileActions files(doc);
    Panels panels;
    if (options->newComponentPrompt)
        panels.openNewComponent(*options->newComponentPrompt);
    if (options->newStateMachinePrompt)
        panels.openNewStateMachine(*options->newStateMachinePrompt);
    SceneViewport viewport(kSceneView);
    GuiBuilder guiBuilder(kGuiPreviewView);
    UiScale uiScale(options->uiScale);
    FrameLimit frames(options->frames, options->screenshot);
    if (options->screenshotOnLog)
        frames.endOnLog(options->screenshotOnLog->first, options->screenshotOnLog->second);
    size_t simLogSeen = 0; // --sim-on-log: log lines already searched
    std::optional<int> simStart; // the frame --sim-mouse / --sim-key frames count from; unset until --sim-on-log's line
    if (options->simOnLog.empty())
        simStart = 0;
    float componentPollSeconds = 0.0f;
    ExternalEditor externalEditor(settingsFile);
    std::string preferencesDraft; // the Preferences modal's edit of the command
    std::string composeDraft;     // and of the compose folder
    OnlinePanel onlinePanel;
    bool showLighting = options->focusPanel == "Lighting"; // hidden by default; --focus-panel Lighting opens it
    bool showPostProcessing = options->focusPanel == "Post Processing"; // the same
    bool showPhysics = options->focusPanel == "Physics";                // the same
    bool showMixer = options->focusPanel == "Mixer";                    // the same
    bool showNetwork = options->focusPanel == "Network";                // the same
    bool showOnline = options->focusPanel == "Online";                  // the same
    if (!options->openExternal.empty())
        externalEditor.open(doc.project->root /
                            std::filesystem::path(reinterpret_cast<const char8_t*>(options->openExternal.c_str())));

    kast::AppHooks hooks;
    SDL_Window* mainWindow = nullptr;
    hooks.init = [&](SDL_Window* window) {
        mainWindow = window;
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        applyTheme();
        loadFont();
        uiScale.init(window);
        if (!ImGui_ImplSDL3_InitForOther(window)) {
            kast::log::error("ImGui SDL3 backend init failed");
            ImGui::DestroyContext();
            return false;
        }
        imgui_bgfx::init(kImGuiView);
        viewport.init();
        viewport.setPostEffects(!options->noScenePost);
        game.init();
        kast::gui::init();
        if (!options->openGui.empty()) {
            std::filesystem::path rml = std::filesystem::path(reinterpret_cast<const char8_t*>(options->openGui.c_str()));
            if (rml.is_relative() && doc.project)
                rml = doc.project->root / rml;
            guiBuilder.open(std::filesystem::absolute(rml).lexically_normal());
            if (!options->guiSelect.empty() && !guiBuilder.selectById(options->guiSelect))
                kast::log::error(std::format("--gui-select {}: no element with that id", options->guiSelect));
        }
        if (options->guiPreviewSize)
            guiBuilder.setPreviewSize(options->guiPreviewSize->first, options->guiPreviewSize->second);
        files.setWindow(window);
        bgfx::setViewClear(kImGuiView, BGFX_CLEAR_COLOR, kBackbufferClearColor);
        kast::log::info("ImGui initialized (docking enabled)");
        return true;
    };
    hooks.event = [&](const SDL_Event& event) {
        uiScale.onEvent(event);
        ImGui_ImplSDL3_ProcessEvent(&event); // ImGui keeps getting input while the game has it, for Ctrl+P
        if (game.hasInput(doc)) {
            doc.input.view = game.inputView();
            doc.input.onEvent(event);
            game.onEvent(event);
        }
    };
    FpsLog fpsLog;
    kast::CursorLock cursorLock;
    hooks.frame = [&] {
        const uint64_t frameStart = SDL_GetPerformanceCounter();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        online.poll(doc.project ? &*doc.project : nullptr, onlinePanel.visible());
        applyWorldOverride();
        if (playAfterLogin && !online.loggingIn()) {
            playAfterLogin = false;
            startPlay(doc);
        }
        updatePlay(doc, ImGui::GetIO().DeltaTime);
        if (game.hasInput(doc)) // otherwise ImGui's backend turns text input on and off for its own fields
            kast::gui::syncTextInput(mainWindow, game.gui().focusedField().has_value());
        componentPollSeconds += ImGui::GetIO().DeltaTime;
        if (componentPollSeconds >= kComponentPollSeconds) { // Lua component hot reload (does nothing during Play)
            componentPollSeconds = 0.0f;
            refreshComponents(doc);
            refreshStateMachines(doc); // also during Play: only the editor's listing (Play reloads its own)
            panels.refreshAssets(doc);
            game.gui().reloadChanged(); // Play-mode GUI hot reload
            guiBuilder.poll();
        }

        bool openPreferences = options->openPreferences && frames.frame() == kAddComponentPopupFrame;
        drawMenuBar(files, doc, frames.frame() == kAddComponentPopupFrame ? options->openMenu : std::string(),
                    openPreferences, showLighting, showPostProcessing, showPhysics, showMixer, showNetwork, showOnline);
        const ImGuiID dockspace = ImGui::GetID("MainDockSpace");
        if (!ImGui::DockBuilderGetNode(dockspace)) // no saved layout in imgui.ini
            buildDefaultLayout(dockspace);
        ImGui::DockSpaceOverViewport(dockspace);

        if (options->selectNetworkEntity && doc.play != PlayState::Stopped && doc.selected == entt::null) {
            const auto clones = doc.scene.registry().view<kast::NetworkEntity>();
            if (!clones.empty())
                doc.selected = clones.front(); // for screenshots of a replicated clone in the Inspector
        }
        if (options->addComponentPopup && frames.frame() == kAddComponentPopupFrame)
            panels.openAddComponent();
        if (options->openBuildSettings && frames.frame() == kAddComponentPopupFrame)
            files.openBuildSettings();
        if (options->openOnlineSettings && frames.frame() == kAddComponentPopupFrame)
            files.openOnlineSettings();
        if (!options->focusPanel.empty() && frames.frame() == kAddComponentPopupFrame) {
            // Applied when that window is submitted below. Builder panels go by their "###" id.
            const std::string id = "###" + options->focusPanel;
            ImGui::SetWindowFocus(ImGui::FindWindowByName(id.c_str()) ? id.c_str() : options->focusPanel.c_str());
        }
        if (!options->openGui.empty() && frames.frame() == kAddComponentPopupFrame)
            guiBuilder.focusPanels(); // again: the first frames' docking undid the focus open() asked for
        if (options->guiPick && frames.frame() == kAddComponentPopupFrame + 6) { // the preview has been laid out
            const auto [x, y] = *options->guiPick;
            if (guiBuilder.pick(static_cast<float>(x), static_cast<float>(y)))
                kast::log::info(std::format("--gui-pick {},{} selected '{}'", x, y, guiBuilder.selectedId()));
            else
                kast::log::error("--gui-pick: the GUI Preview isn't showing");
        }
        if (options->guiAdd && frames.frame() == kAddComponentPopupFrame + 7)
            guiBuilder.requestAdd(*options->guiAdd);
        if (options->guiSave && frames.frame() == kAddComponentPopupFrame + 9)
            guiBuilder.save();
        if (!options->guiClick.empty() && frames.frame() == kAddComponentPopupFrame && !game.gui().click(options->guiClick))
            kast::log::error(std::format("--gui-click {}: no such element in a visible game GUI", options->guiClick));
        if (options->assetsMenu && frames.frame() == kAddComponentPopupFrame + 2) // after --focus-panel, which closes popups
            panels.openAssetMenu(*options->assetsMenu);
        if (options->assetsCreate && frames.frame() == kAddComponentPopupFrame)
            panels.requestAssetCreate(options->assetsCreate->first, options->assetsCreate->second);
        if (!options->assetsSelect.empty() && frames.frame() == kAddComponentPopupFrame)
            panels.selectAsset(options->assetsSelect);
        panels.drawHierarchy(doc);
        // Before the views render: Inspector edits (a material saved to disk) show in this frame's Scene and Game.
        panels.drawInspector(doc);
        viewport.draw(doc); // submitted before Game so Scene is the front tab on first run
        // Layouts saved before the Game panel existed: dock it as a tab beside Scene.
        if (const ImGuiWindowSettings* scene = ImGui::FindWindowSettingsByID(ImHashStr("Scene")))
            ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
        game.draw(doc);
        // Layouts saved before the Systems panel existed: dock it as a tab beside Hierarchy.
        if (const ImGuiWindowSettings* hierarchy = ImGui::FindWindowSettingsByID(ImHashStr("Hierarchy"));
            hierarchy && !ImGui::FindWindowSettingsByID(ImHashStr("Systems")))
            ImGui::SetNextWindowDockID(hierarchy->DockId, ImGuiCond_FirstUseEver);
        panels.drawSystems(doc);
        // Layouts saved before the Components panel existed: dock it as a tab beside Systems.
        if (const ImGuiWindowSettings* systems = ImGui::FindWindowSettingsByID(ImHashStr("Systems"));
            systems && !ImGui::FindWindowSettingsByID(ImHashStr("Components")))
            ImGui::SetNextWindowDockID(systems->DockId, ImGuiCond_FirstUseEver);
        panels.drawComponents(doc);
        panels.drawAssets(doc, files, guiBuilder, externalEditor);
        // Layouts saved before the Events panel existed: dock it as a tab beside Console.
        if (const ImGuiWindowSettings* console = ImGui::FindWindowSettingsByID(ImHashStr("Console"));
            console && !ImGui::FindWindowSettingsByID(ImHashStr("Events")))
            ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
        panels.drawEvents(doc);
        // Layouts saved before the State Machines panel existed: dock it as a tab beside Console.
        if (const ImGuiWindowSettings* console = ImGui::FindWindowSettingsByID(ImHashStr("Console"));
            console && !ImGui::FindWindowSettingsByID(ImHashStr("State Machines")))
            ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
        panels.drawStateMachines(doc);
        panels.drawLighting(doc, showLighting);
        panels.drawPostProcessing(doc, showPostProcessing);
        panels.drawPhysics(doc, showPhysics);
        panels.drawMixer(doc, showMixer);
        panels.drawNetwork(doc, showNetwork);
        onlinePanel.draw(doc, online, files, options->worldAddress, showOnline);
        drawGuiBuilder(guiBuilder);
        drawConsole(); // submitted after Assets, Events and State Machines so Console is the front tab on first run
        drawPreferences(externalEditor, online, preferencesDraft, composeDraft, openPreferences);
        externalEditor.poll();
        files.update();
        if (game.hasInput(doc))
            doc.input.endFrame();
        else
            doc.input.clear();
        if (!options->simMouse) // ponytail: a scripted drag never grabs the real mouse
            cursorLock.sync(mainWindow, doc.input.cursorLocked);
        if (!simStart) { // --sim-on-log: the scripted frames count from the frame the line shows
            const std::vector<std::string>& lines = kast::log::lines();
            for (; simLogSeen < lines.size() && !simStart; ++simLogSeen)
                if (lines[simLogSeen].find(options->simOnLog) != std::string::npos)
                    simStart = frames.frame();
        }
        if (simStart) {
            const int frame = frames.frame() - *simStart;
            if (options->simMouse)
                pushSimMouse(*options->simMouse, frame, game.imageCenter(), SDL_GetWindowID(mainWindow));
            for (const SimKey& key : options->simKeys)
                pushSimKey(key, frame, SDL_GetWindowID(mainWindow));
        }

        ImGui::Render();
        bgfx::touch(kImGuiView);
        darkenTextOnAccent(ImGui::GetDrawData());
        imgui_bgfx::render(ImGui::GetDrawData());
        frames.endFrame();
        if (options->logFps)
            fpsLog.endFrame(frameStart);
    };
    hooks.screenshotSaved = [&](const char*, bool saved) { frames.onScreenshotSaved(saved); };
    hooks.quitRequested = [&] {
        // A --frames run has no one to answer "Unsaved changes" (e.g. after --new-component): it always quits.
        return (guiBuilder.onQuitRequested() && files.onQuitRequested()) || options->frames != 0;
    };
    hooks.shutdown = [&] {
        guiBuilder.shutdown();
        game.shutdown();
        kast::gui::shutdown();
        viewport.shutdown();
        imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        kast::log::info("Shutdown: ImGui");
    };

    const int code = kast::run({.title = "Kast Editor",
                                .width = options->size ? options->size->first : 1280,
                                .height = options->size ? options->size->second : 720,
                                .vsync = !options->noVsync,
                                .activate = options->frames == 0,
                                .position = options->windowPos},
                               hooks);
    if (code != 0)
        return code;
    return frames.failed() ? 1 : 0;
}
