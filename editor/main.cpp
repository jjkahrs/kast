#include "build.h"
#include "document.h"
#include "game_view.h"
#include "imgui_bgfx.h"
#include "launcher.h"
#include "panels.h"
#include "recent_projects.h"
#include "ui_common.h"
#include "viewport.h"

#include <app.h>
#include <log.h>

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <ImGuizmo.h> // after imgui.h, which it needs
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h> // DockBuilder API

#include <charconv>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr bgfx::ViewId kSceneView = 1;
constexpr bgfx::ViewId kGameView = 2;
constexpr bgfx::ViewId kImGuiView = 255; // last, so the UI draws over everything
constexpr uint32_t kBackbufferClearColor = 0x202020ff;
// --add-component-popup, --open-menu, --open-build-settings and --focus-panel wait this long: focus changes while the first frames dock the windows close
// popups.
constexpr int kAddComponentPopupFrame = 10;
constexpr float kComponentPollSeconds = 0.5f; // Lua component and state machine definition files are checked this often
constexpr const char* kUsage =
    "Usage: kast_editor [--frames N] [--screenshot out.png] [--size WxH] [--ui-scale F] [--projects-file list.json] "
    "[--scene in.scene] [--project dir] [--save-scene out.scene] [--select ID] [--play] [--add-component-popup] "
    "[--new-component-prompt Name] [--new-component Name] [--new-state-machine-prompt Name] [--build dir] "
    "[--open-menu Name] [--open-build-settings] "
    "[--focus-panel Window] | "
    "[--launcher] [--new-project name dir] [--launcher-open dir] [-- editor flags...]";

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
    std::optional<std::pair<std::string, std::string>> newProject; // name, dir: open the prefilled New Project modal
    bool addComponentPopup = false; // open the Inspector's Add Component popup for the --select entity
    std::optional<std::string> newComponentPrompt; // open the Inspector's New Lua Component modal with this name
    std::string newComponent; // create this Lua component and add it to the --select entity at startup
    std::optional<std::string> newStateMachinePrompt; // open the State Machines panel's New State Machine modal
    std::string build;        // build the --project game into this folder and exit, without opening a window
    std::string openMenu;     // open this main menu (e.g. "Build") for screenshots
    bool openBuildSettings = false; // open the Build Settings modal (needs --project) for screenshots
    std::string focusPanel;         // bring this window (e.g. "Events") to the front of its dock, for screenshots
    std::string projectsFile;       // the recent-projects list; empty = the one in %APPDATA%
    bool launcher = false;          // run the Project Manager (the default anyway when nothing asks for the editor)
    std::string launcherOpen;       // open this project as the Project Manager would, without a window, and exit
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
        } else if (arg == "--open-menu" && hasValue) {
            options.openMenu = argv[++i];
        } else if (arg == "--focus-panel" && hasValue) {
            options.focusPanel = argv[++i];
        } else if (arg == "--projects-file" && hasValue) {
            options.projectsFile = argv[++i];
        } else if (arg == "--launcher") {
            options.launcher = true;
        } else if (arg == "--launcher-open" && hasValue) {
            options.launcherOpen = argv[++i];
        } else if (arg == "--") {
            options.childArgs.assign(argv + i + 1, argv + argc);
            break;
        } else if (arg == "--open-build-settings") {
            options.openBuildSettings = true;
        } else if (arg == "--play") {
            options.play = true;
        } else if (arg == "--size" && hasValue) {
            const std::string_view size = argv[++i];
            const size_t x = size.find('x');
            int width = 0;
            int height = 0;
            if (x == std::string_view::npos || !parsePositive(size.substr(0, x), width) ||
                !parsePositive(size.substr(x + 1), height)) {
                kast::log::error(std::format("Invalid --size '{}', expected WxH", size));
                return std::nullopt;
            }
            options.size.emplace(width, height);
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
                             options.openBuildSettings || !options.focusPanel.empty() || options.newStateMachinePrompt;
    if (!options.editor() && editorFlags) {
        kast::log::error("--select, --play, --add-component-popup, --new-component[-prompt], "
                         "--new-state-machine-prompt, --open-menu, --open-build-settings and --focus-panel require "
                         "--project <dir> or --scene <file>");
        return std::nullopt;
    }
    if (!options.saveScene.empty() && options.project.empty() && options.scene.empty()) {
        kast::log::error("--save-scene requires --scene <file> or --project <dir>");
        return std::nullopt;
    }
    if (options.openBuildSettings && options.project.empty()) {
        kast::log::error("--open-build-settings requires --project <dir>");
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
    ImGui::DockBuilderFinish(dockspace);
}

// openMenu: a main menu to open this frame (--open-menu), or empty.
void drawMenuBar(FileActions& files, Document& doc, const std::string& openMenu)
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
            ImGui::Separator();
            if (ImGui::MenuItem("Build...", nullptr, false, canBuild))
                files.build();
            if (ImGui::MenuItem("Build and Run", nullptr, false, canBuild))
                files.buildAndRun();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void drawPlaceholder(const char* name)
{
    ImGui::Begin(name);
    ImGui::End();
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
    const std::optional<Options> options = parseArgs(argc, argv);
    if (!options)
        return 2;
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
    refreshStateMachines(doc); // the Inspector and State Machines panel list them from the first frame
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
    GameViewport game(kGameView);
    UiScale uiScale(options->uiScale);
    FrameLimit frames(options->frames, options->screenshot);
    float componentPollSeconds = 0.0f;

    kast::AppHooks hooks;
    hooks.init = [&](SDL_Window* window) {
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();
        loadFont();
        uiScale.init(window);
        if (!ImGui_ImplSDL3_InitForOther(window)) {
            kast::log::error("ImGui SDL3 backend init failed");
            ImGui::DestroyContext();
            return false;
        }
        imgui_bgfx::init(kImGuiView);
        viewport.init();
        game.init();
        files.setWindow(window);
        bgfx::setViewClear(kImGuiView, BGFX_CLEAR_COLOR, kBackbufferClearColor);
        kast::log::info("ImGui initialized (docking enabled)");
        return true;
    };
    hooks.event = [&](const SDL_Event& event) {
        uiScale.onEvent(event);
        ImGui_ImplSDL3_ProcessEvent(&event); // ImGui keeps getting input while the game has it, for Ctrl+P
        if (game.hasInput(doc))
            doc.input.onEvent(event);
    };
    hooks.frame = [&] {
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        updatePlay(doc, ImGui::GetIO().DeltaTime);
        componentPollSeconds += ImGui::GetIO().DeltaTime;
        if (componentPollSeconds >= kComponentPollSeconds) { // Lua component hot reload (does nothing during Play)
            componentPollSeconds = 0.0f;
            refreshComponents(doc);
            refreshStateMachines(doc); // also during Play: only the editor's listing (Play reloads its own)
        }

        drawMenuBar(files, doc, frames.frame() == kAddComponentPopupFrame ? options->openMenu : std::string());
        const ImGuiID dockspace = ImGui::GetID("MainDockSpace");
        if (!ImGui::DockBuilderGetNode(dockspace)) // no saved layout in imgui.ini
            buildDefaultLayout(dockspace);
        ImGui::DockSpaceOverViewport(dockspace);

        if (options->addComponentPopup && frames.frame() == kAddComponentPopupFrame)
            panels.openAddComponent();
        if (options->openBuildSettings && frames.frame() == kAddComponentPopupFrame)
            files.openBuildSettings();
        if (!options->focusPanel.empty() && frames.frame() == kAddComponentPopupFrame)
            ImGui::SetWindowFocus(options->focusPanel.c_str()); // applied when that window is submitted below
        panels.drawHierarchy(doc);
        viewport.draw(doc); // submitted before Game so Scene is the front tab on first run
        // Layouts saved before the Game panel existed: dock it as a tab beside Scene.
        if (const ImGuiWindowSettings* scene = ImGui::FindWindowSettingsByID(ImHashStr("Scene")))
            ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
        game.draw(doc);
        panels.drawInspector(doc);
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
        drawPlaceholder("Assets");
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
        drawConsole(); // submitted after Assets, Events and State Machines so Console is the front tab on first run
        files.update();
        if (game.hasInput(doc))
            doc.input.endFrame();
        else
            doc.input.clear();

        ImGui::Render();
        bgfx::touch(kImGuiView);
        imgui_bgfx::render(ImGui::GetDrawData());
        frames.endFrame();
    };
    hooks.screenshotSaved = [&](const char*, bool saved) { frames.onScreenshotSaved(saved); };
    hooks.quitRequested = [&] {
        // A --frames run has no one to answer "Unsaved changes" (e.g. after --new-component): it always quits.
        return files.onQuitRequested() || options->frames != 0;
    };
    hooks.shutdown = [&] {
        game.shutdown();
        viewport.shutdown();
        imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        kast::log::info("Shutdown: ImGui");
    };

    const int code = kast::run({.title = "Kast Editor",
                                .width = options->size ? options->size->first : 1280,
                                .height = options->size ? options->size->second : 720},
                               hooks);
    if (code != 0)
        return code;
    return frames.failed() ? 1 : 0;
}
