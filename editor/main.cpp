#include "build.h"
#include "document.h"
#include "external_editor.h"
#include "game_view.h"
#include "imgui_bgfx.h"
#include "launcher.h"
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
    "Usage: kast_editor [--frames N] [--screenshot out.png] [--size WxH] [--ui-scale F] [--projects-file list.json] "
    "[--scene in.scene] [--project dir] [--save-scene out.scene] [--select ID] [--play] [--no-scene-post] [--add-component-popup] "
    "[--new-component-prompt Name] [--new-component Name] [--new-state-machine-prompt Name] [--build dir] "
    "[--open-menu Name] [--open-build-settings] [--open-online-settings] [--assets-menu path|/] [--assets-create system|fsm|gui|material:folder] [--assets-select path] "
    "[--focus-panel Window] [--open-gui file.rml] [--gui-select id] [--gui-preview-size WxH] [--gui-pick X,Y] "
    "[--gui-add Panel|Text|Button] [--gui-save] [--gui-click id] [--settings-file settings.json] [--open-preferences] "
    "[--open-external path] [--drop-model path] [--world-address host:port] [--log-fps] [--no-vsync] [--no-audio-device] [--crash abort|assert] | "
    "[--launcher] [--new-project name dir] [--launcher-open dir] [-- editor flags...]";

// --log-fps: once a second, the frame rate (vsync caps it at the display's refresh rate) and what a frame costs below
// that cap: the editor's CPU time per frame (update, UI and submitting draws) and bgfx's last GPU frame time.
class FpsLog {
public:
    void endFrame(uint64_t frameStart)
    {
        const uint64_t now = SDL_GetPerformanceCounter();
        const auto frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        cpuSeconds_ += static_cast<double>(now - frameStart) / frequency;
        ++frames_;
        if (windowStart_ == 0)
            windowStart_ = now;
        const double elapsed = static_cast<double>(now - windowStart_) / frequency;
        if (elapsed < 1.0)
            return;
        const bgfx::Stats* stats = bgfx::getStats();
        const double gpuMs = 1000.0 * static_cast<double>(stats->gpuTimeEnd - stats->gpuTimeBegin) /
                             static_cast<double>(stats->gpuTimerFreq);
        kast::log::info(std::format("FPS {:.1f}, CPU {:.2f} ms/frame, GPU {:.2f} ms/frame, {} draw calls", frames_ / elapsed,
                                    1000.0 * cpuSeconds_ / frames_, gpuMs, stats->numDraw));
        windowStart_ = now;
        frames_ = 0;
        cpuSeconds_ = 0.0;
    }

private:
    uint64_t windowStart_ = 0;
    int frames_ = 0;
    double cpuSeconds_ = 0.0;
};

struct Options {
    int frames = 0;         // 0 = run until quit
    std::string screenshot; // captured after the last frame
    std::optional<std::pair<int, int>> size; // window size; default 1280x720 (editor), 1024x600 (Project Manager)
    std::optional<float> uiScale; // overrides the monitor's display scale
    std::string project;          // project directory opened at startup (before --scene)
    std::string scene;            // opened at startup instead of the default scene
    std::string saveScene;        // save the startup scene here and exit, without opening a window
    kast::EntityId select = 0;    // select this entity at startup; 0 = none
    bool play = false;            // enter Play mode once the startup scene is loaded
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
    std::string focusPanel;         // bring this window (e.g. "Events"; "Lighting", "Post Processing", "Physics", "Mixer" and "Network" also open it) to the front, for screenshots
    std::string openGui;            // open this .rml in the GUI Builder (relative paths: to the project, else the cwd)
    std::string guiSelect;          // then select the element with this id in it
    std::optional<std::pair<int, int>> guiPreviewSize; // add this GUI Preview resolution and pick it
    std::optional<std::pair<int, int>> guiPick; // click the GUI Preview here (preview pixels, kAddComponentPopupFrame)
    std::optional<GuiKind> guiAdd; // then add this element as the GUI Hierarchy's + buttons do
    bool guiSave = false;          // then save the GUI as its Save button does
    std::string guiClick;           // with --play: click the game GUI element with this id (frame kAddComponentPopupFrame)
    std::string projectsFile;       // the recent-projects list; empty = the one in %APPDATA%
    std::string settingsFile;       // the editor settings (external editor command); empty = the one in %APPDATA%
    bool openPreferences = false;   // open the File > Preferences modal for screenshots
    std::string openExternal;       // open this project file (path relative to the project) in the external editor
    bool launcher = false;          // run the Project Manager (the default anyway when nothing asks for the editor)
    std::string launcherOpen;       // open this project as the Project Manager would, without a window, and exit
    std::string dropModel;          // add this project model file as dropping it from the Assets panel does
    std::string worldAddress;       // host:port replacing every realm's world address in Play (kast.world), editor only
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
        } else if (arg == "--no-scene-post") {
            options.noScenePost = true;
        } else if (arg == "--size" && hasValue) {
            if (!parseSize(argv[++i], options.size))
                return std::nullopt;
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
    const bool editorFlags = options.select != 0 || options.play || options.addComponentPopup ||
                             options.newComponentPrompt || !options.newComponent.empty() || !options.openMenu.empty() ||
                             options.openBuildSettings || options.openOnlineSettings || !options.focusPanel.empty() || options.newStateMachinePrompt ||
                             options.assetsMenu || options.assetsCreate || !options.assetsSelect.empty() ||
                             !options.openGui.empty() ||
                             !options.guiSelect.empty() || options.guiPreviewSize || options.guiPick ||
                             options.guiAdd || options.guiSave || !options.guiClick.empty() || options.openPreferences ||
                             !options.openExternal.empty() || !options.dropModel.empty() || options.logFps ||
                             options.noVsync || !options.worldAddress.empty();
    if (!options.editor() && editorFlags) {
        kast::log::error("--select, --play, --add-component-popup, --new-component[-prompt], "
                         "--new-state-machine-prompt, --open-menu, --open-build-settings, --open-online-settings, --focus-panel, "
                         "--assets-menu, "
                         "--assets-create, --assets-select, --open-gui, --gui-select, --gui-preview-size, --gui-pick, --gui-add, --gui-save, "
                         "--gui-click, --open-preferences, --open-external and --world-address require "
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
    if (!options.screenshot.empty() && options.frames == 0) {
        kast::log::error("--screenshot requires --frames N");
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
// chosen. showLighting, showPostProcessing, showPhysics, showMixer, showNetwork: those panels' visibility, toggled in the
// Window menu.
void drawMenuBar(FileActions& files, Document& doc, const std::string& openMenu, bool& openPreferences,
                 bool& showLighting, bool& showPostProcessing, bool& showPhysics, bool& showMixer, bool& showNetwork)
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
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// File > Preferences...: the external editor command (per user, every project). open: open it this frame, showing the
// current setting.
void drawPreferences(ExternalEditor& editor, std::string& draft, bool open)
{
    if (open) {
        draft = editor.command();
        ImGui::OpenPopup("Preferences");
    }
    if (!ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("External editor command");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 25.0f);
    ImGui::InputText("##externalEditor", &draft);
    ImGui::TextDisabled("{file} is replaced with the file's full path. Default: %s", kDefaultEditorCommand);
    if (ImGui::Button("Save")) {
        if (std::string error; editor.setCommand(draft, error))
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
    if (!options->worldAddress.empty())
        doc.lua.world().setAddressOverride(options->worldAddress); // kept by every Play start
    if (options->play)
        startPlay(doc);
    if (std::string error; opened && !recordRecent(projectsFile, *opened, error))
        kast::log::warn(error);

    FileActions files(doc);
    Panels panels;
    if (options->newComponentPrompt)
        panels.openNewComponent(*options->newComponentPrompt);
    if (options->newStateMachinePrompt)
        panels.openNewStateMachine(*options->newStateMachinePrompt);
    SceneViewport viewport(kSceneView);
    GameViewport game(kGameView, kGameGuiView);
    GuiBuilder guiBuilder(kGuiPreviewView);
    UiScale uiScale(options->uiScale);
    FrameLimit frames(options->frames, options->screenshot);
    float componentPollSeconds = 0.0f;
    ExternalEditor externalEditor(options->settingsFile.empty() ? defaultEditorSettingsFile()
                                                                : std::filesystem::absolute(options->settingsFile));
    std::string preferencesDraft; // the Preferences modal's edit of the command
    bool showLighting = options->focusPanel == "Lighting"; // hidden by default; --focus-panel Lighting opens it
    bool showPostProcessing = options->focusPanel == "Post Processing"; // the same
    bool showPhysics = options->focusPanel == "Physics";                // the same
    bool showMixer = options->focusPanel == "Mixer";                    // the same
    bool showNetwork = options->focusPanel == "Network";                // the same
    if (!options->openExternal.empty())
        externalEditor.open(doc.project->root /
                            std::filesystem::path(reinterpret_cast<const char8_t*>(options->openExternal.c_str())));

    kast::AppHooks hooks;
    hooks.init = [&](SDL_Window* window) {
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
            doc.input.onEvent(event);
            game.onEvent(event);
        }
    };
    FpsLog fpsLog;
    hooks.frame = [&] {
        const uint64_t frameStart = SDL_GetPerformanceCounter();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        updatePlay(doc, ImGui::GetIO().DeltaTime);
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
                    openPreferences, showLighting, showPostProcessing, showPhysics, showMixer, showNetwork);
        const ImGuiID dockspace = ImGui::GetID("MainDockSpace");
        if (!ImGui::DockBuilderGetNode(dockspace)) // no saved layout in imgui.ini
            buildDefaultLayout(dockspace);
        ImGui::DockSpaceOverViewport(dockspace);

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
        drawGuiBuilder(guiBuilder);
        drawConsole(); // submitted after Assets, Events and State Machines so Console is the front tab on first run
        drawPreferences(externalEditor, preferencesDraft, openPreferences);
        externalEditor.poll();
        files.update();
        if (game.hasInput(doc))
            doc.input.endFrame();
        else
            doc.input.clear();

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
                                .activate = options->frames == 0},
                               hooks);
    if (code != 0)
        return code;
    return frames.failed() ? 1 : 0;
}
