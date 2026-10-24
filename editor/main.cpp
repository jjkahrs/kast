#include "build.h"
#include "document.h"
#include "editor_settings.h"
#include "external_editor.h"
#include "game_view.h"
#include "imgui_bgfx.h"
#include "launcher.h"
#include "model_importer.h"
#include "model_preview.h"
#include "online.h"
#include "online_panel.h"
#include "lightmap_job.h"
#include "voxelgi_job.h"
#include "panels.h"
#include "recent_projects.h"
#include "shader_compiler.h"
#include "shader_graph_editor.h"
#include "texture_cooker.h"
#include "ui_common.h"
#include "viewport.h"
#include "zone_cli.h"
#include "zone_panel.h"
#include "zone_render.h"
#include "zone_session.h"

#include <app.h>
#include <audio.h>
#include <components.h>
#include <crash_dialogs.h>
#include <gui.h>
#include <log.h>
#include <prefab.h>
#include <renderer.h>
#include <sdfgi.h>
#include <world_client.h>

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <ImGuizmo.h> // after imgui.h, which it needs
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h> // DockBuilder API
#include <imgui_stdlib.h>

#include <array>
#include <cassert>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr float kZoneNearPlane = 0.5f; // the editor camera's planes while the Scene view shows a zone
constexpr float kZoneFarPlane = 6000.0f;
// Each viewport's shadow maps, then its scene and post passes (see SceneViewport, GameViewport).
constexpr bgfx::ViewId kViewportViews = kast::Renderer::kShadowViews + kast::PostProcess::kViews;
constexpr bgfx::ViewId kSceneView = 0; // then its overlays (view 0 too: 256 views hold no more)
constexpr bgfx::ViewId kGameView = kSceneView + kViewportViews + 1;
constexpr bgfx::ViewId kGameGuiView = kGameView + kViewportViews; // the game GUIs (kast::gui::kViews), over the Game view's scene
constexpr bgfx::ViewId kGuiPreviewView = kGameGuiView + kast::gui::kViews; // the GUI Builder's preview
constexpr bgfx::ViewId kShaderPreviewView = kGuiPreviewView + kast::gui::kViews; // the Shader Graph's preview
constexpr bgfx::ViewId kModelPreviewView = kShaderPreviewView + ShaderPreview::kViews; // the Model Preview
constexpr bgfx::ViewId kImGuiView = 255; // last, so the UI draws over everything
static_assert(kModelPreviewView + ModelPreview::kViews <= kImGuiView, "the preview views must end before ImGui's");
constexpr uint32_t kBackbufferClearColor = 0x202020ff;
// --add-component-popup, --open-menu, --open-build-settings, --open-online-settings, --open-preferences, --focus-panel and --assets-* wait this long: focus changes while the first frames dock the windows close
// popups.
constexpr int kAddComponentPopupFrame = 10;
constexpr float kComponentPollSeconds = 0.5f; // Lua definition files and the Assets tree are checked this often
constexpr const char* kUsage =
    "Usage: kast_editor [--frames N] [--screenshot out.png] [--screenshot-on-log frames,text] [--size WxH] [--window-pos X,Y] [--ui-scale F] [--projects-file list.json] "
    "[--scene in.scene] [--project dir] [--save-scene out.scene] [--select ID] [--play] [--select-network-entity] [--no-scene-post] [--add-component-popup] "
    "[--new-component-prompt Name] [--new-component Name] [--new-state-machine-prompt Name] [--build dir] "
    "[--open-menu Name] [--open-build-settings] [--open-online-settings] [--assets-menu path|/] [--assets-create system|fsm|gui|material|shader|shadergraph:folder] [--assets-select path] [--material-set field=json] [--material-undo N] [--material-redo N] "
    "[--focus-panel Window] [--zone-section Map[,Problems...]] [--zone-tool Raise|Lower|Flatten|SetSpawn|...] [--zone-select id|object:tx,tz,index] [--bake] [--mapbake exe] [--open-gui file.rml] [--gui-select id] [--gui-preview-size WxH] [--gui-pick X,Y] "
    "[--gui-add Panel|Text|RichText|Button|Toggle|Slider|Dropdown|Spinbox|Progress|ListBox|VirtualList] [--gui-set-attr name=value] [--gui-set-prop name=value] [--gui-rich-wrap tag[=value],start,end] [--gui-undo N] [--gui-redo N] [--gui-save] [--gui-click id] "
    "[--open-graph file.kshadergraph] [--graph-add type[,x,y]] [--graph-link N.pin,M.pin] [--graph-unlink N.pin] [--graph-select N[,M...]] "
    "[--graph-delete N[,M...]] [--graph-set N.setting=json] [--graph-comment title,x,y,w,h] [--graph-paste x,y] [--graph-undo N] [--graph-redo N] "
    "[--graph-save] [--graph-add-menu N.pin|/] [--graph-preview sphere|cube|plane] [--graph-view-code] [--preview-model path] [--preview-clip name] [--preview-lod N] "
    "[--sim-mouse frame,none|left|right[+shift],dx,dy,wheel[,sx,sy[,hold]]] [--sim-key frame,[ctrl+]key]... [--sim-on-log text] [--settings-file settings.json] [--open-preferences] "
    "[--open-external path] [--drop-model path] [--create-prefab folder|/] [--drop-prefab path[,parentId]] [--drop-prefab-view path,u,v] "
    "[--set-field ID,Component.field,json] [--prefab-revert spec] [--prefab-apply spec] [--prefab-unpack ID] [--edit-prefab path] "
    "[--prefab-save] [--prefab-back] [--scene-undo N] [--scene-redo N] [--assets-delete path] [--inspector-apply-menu Component.field] [--prefab-back-prompt] [--world-address host:port] [--online-login-env] [--online-compose up|down|ps] [--log-fps] [--log-sdfgi] [--fixed-dt S] [--no-vsync] [--no-instancing] [--no-culling] [--no-audio-device] [--crash abort|assert] [--file-save] [--bake-probes] [--bake-lightmaps] [--bake-lightmaps-async] [--cancel-lightmaps N] [--bake-voxelgi] [zone edits] | "
    "[--launcher] [--new-project name dir] [--launcher-open dir] [-- editor flags...]";

// --sim-mouse: a scripted mouse drag and/or wheel turn on the Game panel (without --play: on the Scene view), for
// screenshots of mouse-driven games and tests of Scene view clicks (the Zone tab tools).
struct SimMouse {
    int frame = 0;  // the press (at the Game image's center) and the wheel go out on this frame
    int button = 0; // SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT, 0 = none
    float dx = 0.0f; // moved the frame after the press; released the frame after that
    float dy = 0.0f;
    float wheel = 0.0f; // notches, + = away from the user
    float sx = 0.0f;    // where the press goes, from the Game image's center (",sx,sy" after wheel)
    float sy = 0.0f;
    int hold = 1;       // frames the button stays down after the move (",hold" after sy): a drag that waits there
    bool shift = false; // "left+shift": Shift reads as held from the press until after the release
};

// --sim-key: one key press on the Game panel, or the focused panel (down, its typed character, up two frames later),
// for tests of game keys and the Shader Graph canvas' shortcuts.
struct SimKey {
    int frame = 0;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    SDL_Keycode key = SDLK_UNKNOWN;
    std::string text; // what typing it inserts into a focused text field ("b"); empty for keys that type nothing
    bool ctrl = false; // "ctrl+C": Ctrl reads as held with the key down
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
    std::vector<std::pair<std::string, nlohmann::ordered_json>> materialSets; // --material-set: Inspector edits of that .mat, in order
    int materialUndo = 0, materialRedo = 0; // --material-undo N / --material-redo N: then material Undo N times, then Redo N times
    std::optional<std::pair<Panels::AssetCreate, std::string>> assetsCreate; // run Assets Create in that folder
    bool openBuildSettings = false; // open the Build Settings modal (needs --project) for screenshots
    bool openOnlineSettings = false; // open the Online Services modal (needs --project) for screenshots
    std::vector<std::string> zoneSections; // --zone-section: expand these Zone tab sections, collapse the others (with --focus-panel Zone), for screenshots
    std::optional<ZonePanel::Tool> zoneTool; // --zone-tool: this Zone tab tool is active (tests, with --sim-mouse)
    std::string zoneSelect; // --zone-select: select this zone area or spawn id, or "object:tx,tz,index"
    bool bake = false;      // --bake: Bake Navmesh on the zone (--frames counts once it ends; exit 1 if it fails)
    bool bakeProbes = false; // --bake-probes: Window > Lighting's Bake All Probes, before --file-save's frame
    // --bake-lightmaps: Window > Lighting's Bake Lightmaps, waited for, before --file-save's frame. --bake-lightmaps-async:
    // started there but not waited for (the editor draws meanwhile); --cancel-lightmaps N: its Cancel at frame N.
    bool bakeLightmaps = false;
    bool bakeLightmapsAsync = false;
    int cancelLightmaps = 0;
    bool bakeVoxelGi = false; // --bake-voxelgi: Window > Lighting's Bake VoxelGI, waited for, before --file-save's frame
    std::string mapbake;    // --mapbake: the kast_mapbake.exe for this run (not saved to the settings)
    std::vector<zone_cli::Op> zoneOps; // --map, --new-map, --sculpt, ..., --save-zone (zone_cli.h), in order, before the window
    bool headless = false;  // --headless: run the zone edits (and --bake) and exit without opening a window
    bool noRecycle = false; // --no-recycle: zone saves delete files instead of sending them to the Recycle Bin
    bool sculptLoop = false; // --sculpt-loop: a moving brush sculpts the zone every frame (the --log-fps benchmark)
    std::optional<std::array<float, 3>> focus; // --focus x,z,distance: the Scene camera's pivot on the zone ground
    std::optional<std::array<int, 2>> promptRemoveTile; // --prompt-remove-tile: open the Zone tab's remove-tile dialog
    bool fileSave = false;  // --file-save: File > Save (as Ctrl+S) at frame kAddComponentPopupFrame, for tests
    int undo = 0, redo = 0; // --scene-undo N / --scene-redo N: scene Undo N times, then Redo N times, after the startup edits
    std::string focusPanel;         // bring this window (e.g. "Events"; "Lighting", "Post Processing", "Physics", "Mixer", "Network" and "Online" also open it) to the front, for screenshots
    std::string openGui;            // open this .rml in the GUI Builder (relative paths: to the project, else the cwd)
    std::string guiSelect;          // then select the element with this id in it
    std::optional<std::pair<int, int>> guiPreviewSize; // add this GUI Preview resolution and pick it
    std::optional<std::pair<int, int>> guiPick; // click the GUI Preview here (preview pixels, kAddComponentPopupFrame)
    std::optional<GuiKind> guiAdd; // then add this element as the GUI Hierarchy's + buttons do
    std::optional<std::pair<std::string, std::string>> guiSetAttr; // then set this attribute of the selection, as the
                                                                   // GUI Inspector does (one step)
    std::optional<std::pair<std::string, std::string>> guiSetProp; // then set this widget property of the selection, as
                                                                   // the GUI Inspector does (one step)
    struct GuiRichWrap {
        std::string tag, value;
        size_t start = 0, end = 0;
    };
    std::optional<GuiRichWrap> guiRichWrap; // then wrap the selected Rich Text's bytes start..end as its toolbar does
    int guiUndo = 0, guiRedo = 0;  // then GUI Undo N times, then Redo N times
    bool guiSave = false;          // then save the GUI as its Save button does
    std::string guiClick;           // with --play: click the game GUI element with this id (frame kAddComponentPopupFrame)
    std::string openGraph;          // open this .kshadergraph in the Shader Graph panels (relative: to the project, else the cwd)
    // --graph-* edits of it (and later --open-graph), flag and value in command-line order, run as the panels do them
    // (runGraphStep) once the canvas is laid out.
    std::vector<std::pair<std::string, std::string>> graphSteps;
    std::string previewModel; // open this model (project-relative) in the Model Preview at startup
    std::string previewClip;  // and play this clip of it instead of its default
    int previewLod = 0;       // and show this LOD of it
    std::optional<SimMouse> simMouse; // push these SDL mouse events on the Game panel (--play) or Scene view (the cursor lock isn't applied)
    std::vector<SimKey> simKeys;      // with --play: these key presses
    std::string simOnLog;             // --sim-on-log: simMouse's and simKeys' frames count from the first log line with this
    std::string projectsFile;       // the recent-projects list; empty = the one in %APPDATA%
    std::string settingsFile;       // the editor settings (editor_settings.h); empty = the one in %APPDATA%
    bool openPreferences = false;   // open the File > Preferences modal for screenshots
    std::string openExternal;       // open this project file (path relative to the project) in the external editor
    bool launcher = false;          // run the Project Manager (the default anyway when nothing asks for the editor)
    std::string launcherOpen;       // open this project as the Project Manager would, without a window, and exit
    std::string dropModel;          // add this project model file as dropping it from the Assets panel does
    // Prefab steps (--create-prefab, --drop-prefab, --drop-prefab-view, --set-field, --prefab-revert, --prefab-apply,
    // --prefab-unpack, --edit-prefab, --prefab-save, --prefab-back): flag and value, run in command-line order at startup
    // (after --select, before --save-scene).
    std::vector<std::pair<std::string, std::string>> prefabSteps;
    std::string assetsDelete;       // open the Assets Delete Asset confirmation for this entry
    std::string inspectorApplyMenu; // open the --select entity's Inspector override menu of "Component.field" (+ Apply)
    bool prefabBackPrompt = false;  // press the prefab edit mode Back button (its unsaved-changes prompt) for screenshots
    std::string worldAddress;       // host:port replacing every realm's world address in Play (kast.world), editor only
    bool onlineLoginEnv = false;    // Online panel test login from KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD (--play waits for it)
    std::optional<OnlineModel::Compose> onlineCompose; // press the Online panel's Start, Stop or Status at startup
    bool logFps = false;            // log frames per second, editor CPU and GPU frame times once a second
    std::string crash;              // --crash abort|assert: fail on purpose, to test KAST_NO_CRASH_DIALOGS
    float fixedDt = 0.0f;           // --fixed-dt: Play and the edit mode preview advance this many seconds a frame (0: real time)
    bool noVsync = false;           // present as fast as possible, to measure the frame rate past the display's
    bool noInstancing = false;      // the Scene and Game views draw every mesh one by one (Renderer::setInstancing)
    bool noCulling = false;         // the Scene and Game views draw every mesh in every pass (Renderer::setCulling)
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
// (offset by sx, sy) presses (and turns the wheel), then moves by dx, dy, then releases there hold frames later (so a
// GUI sees a drag and drop, and the Shader Graph canvas a drag that waits over a pin).
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
        kast::log::info(std::format("--sim-mouse: press at {:.0f},{:.0f}", x, y));
        push(SDL_EVENT_MOUSE_MOTION);
        if (sim.button)
            push(SDL_EVENT_MOUSE_BUTTON_DOWN);
        if (sim.wheel != 0.0f)
            push(SDL_EVENT_MOUSE_WHEEL);
    } else if (frame == sim.frame + 1 || (!sim.button && frame > sim.frame)) { // no button: a hover, held there
        push(SDL_EVENT_MOUSE_MOTION, frame == sim.frame + 1 ? sim.dx : 0.0f, frame == sim.frame + 1 ? sim.dy : 0.0f);
    } else if (sim.button && frame == sim.frame + 1 + sim.hold) {
        push(SDL_EVENT_MOUSE_BUTTON_UP);
    }
    if (sim.shift && (frame == sim.frame || frame == sim.frame + 2 + sim.hold))
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
    const bool ok = (parts.size() == 5 || ((parts.size() == 7 || parts.size() == 8) && number(parts[5], sim.sx) &&
                                           number(parts[6], sim.sy) && (parts.size() == 7 || parsePositive(parts[7], sim.hold)))) &&
                    parsePositive(parts[0], sim.frame) && (button == "none" || button == "left" || button == "right") &&
                    number(parts[2], sim.dx) && number(parts[3], sim.dy) && number(parts[4], sim.wheel);
    if (!ok) {
        kast::log::error(
            std::format("Invalid --sim-mouse '{}', expected frame,none|left|right[+shift],dx,dy,wheel[,sx,sy[,hold]]", text));
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
                 .key = sim.key, .mod = static_cast<SDL_Keymod>(down && sim.ctrl ? SDL_KMOD_LCTRL : SDL_KMOD_NONE),
                 .down = down};
    SDL_PushEvent(&event);
    if (down && !sim.text.empty()) {
        SDL_Event text{};
        text.text = {.type = SDL_EVENT_TEXT_INPUT, .windowID = window, .text = sim.text.c_str()};
        SDL_PushEvent(&text);
    }
}

// "frame,key" with a positive frame and an SDL key name ("B", "Escape"), "ctrl+" before it for a Ctrl shortcut; logs
// why not.
bool parseSimKey(std::string_view text, std::vector<SimKey>& out)
{
    const size_t comma = text.find(',');
    SimKey sim;
    if (comma != std::string_view::npos && parsePositive(text.substr(0, comma), sim.frame)) {
        std::string_view name = text.substr(comma + 1);
        sim.ctrl = name.starts_with("ctrl+");
        if (sim.ctrl)
            name.remove_prefix(5);
        sim.scancode = SDL_GetScancodeFromName(std::string(name).c_str());
    }
    if (sim.scancode == SDL_SCANCODE_UNKNOWN) {
        kast::log::error(std::format("Invalid --sim-key '{}', expected frame,[ctrl+]key (an SDL key name such as B)", text));
        return false;
    }
    sim.key = SDL_GetKeyFromScancode(sim.scancode, SDL_KMOD_NONE, false);
    if (sim.key >= ' ' && sim.key < 0x7f && !sim.ctrl)
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

// "Panel", "Text", "Button", "Toggle", "Slider", "Dropdown", "Spinbox" or "Progress".
// A kind's label, its spaces optional ("List Box", "ListBox").
bool parseGuiKind(std::string_view label, std::optional<GuiKind>& out)
{
    for (GuiKind kind : kGuiCreatableKinds) {
        std::string joined = guiKindLabel(kind);
        std::erase(joined, ' ');
        if (label == guiKindLabel(kind) || label == joined) {
            out = kind;
            return true;
        }
    }
    return false;
}

// "system:<folder>", "fsm:<folder>", "gui:<folder>", "material:<folder>", "shader:<folder>" or "shadergraph:<folder>".
bool parseAssetCreate(std::string_view value, std::optional<std::pair<Panels::AssetCreate, std::string>>& out)
{
    constexpr std::pair<std::string_view, Panels::AssetCreate> kinds[] = {{"system:", Panels::AssetCreate::System},
                                                                          {"fsm:", Panels::AssetCreate::StateMachine},
                                                                          {"gui:", Panels::AssetCreate::Gui},
                                                                          {"material:", Panels::AssetCreate::Material},
                                                                          {"shader:", Panels::AssetCreate::Shader},
                                                                          {"shadergraph:", Panels::AssetCreate::ShaderGraph}};
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
        } else if (arg == "--material-set" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t equals = text.find('=');
            nlohmann::ordered_json value = nlohmann::ordered_json::parse(
                text.substr(equals == std::string_view::npos ? text.size() : equals + 1), nullptr, false);
            if (equals == std::string_view::npos || equals == 0 || value.is_discarded()) {
                kast::log::error(std::format("Invalid --material-set '{}', expected field=json", text));
                return std::nullopt;
            }
            options.materialSets.emplace_back(std::string(text.substr(0, equals)), std::move(value));
        } else if (arg == "--material-undo" && hasValue && parsePositive(argv[i + 1], options.materialUndo)) {
            ++i;
        } else if (arg == "--material-redo" && hasValue && parsePositive(argv[i + 1], options.materialRedo)) {
            ++i;
        } else if (arg == "--assets-create" && hasValue && parseAssetCreate(argv[i + 1], options.assetsCreate)) {
            ++i;
        } else if (arg == "--open-gui" && hasValue) {
            options.openGui = argv[++i];
        } else if (arg == "--preview-model" && hasValue) {
            options.previewModel = argv[++i];
        } else if (arg == "--preview-lod" && hasValue) {
            const std::string_view text = argv[++i];
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), options.previewLod);
            if (ec != std::errc{} || end != text.data() + text.size() || options.previewLod < 0) {
                kast::log::error(std::format("Invalid --preview-lod '{}', expected a level (0, 1, ...)", text));
                return std::nullopt;
            }
        } else if (arg == "--preview-clip" && hasValue) {
            options.previewClip = argv[++i];
        } else if (arg == "--open-graph" && hasValue && options.openGraph.empty()) {
            options.openGraph = argv[++i];
        } else if ((arg == "--open-graph" || arg == "--graph-add" || arg == "--graph-link" || arg == "--graph-unlink" ||
                    arg == "--graph-select" || arg == "--graph-delete" || arg == "--graph-set" || arg == "--graph-comment" ||
                    arg == "--graph-paste" || arg == "--graph-add-menu") &&
                   hasValue) {
            options.graphSteps.emplace_back(arg, argv[++i]);
        } else if ((arg == "--graph-undo" || arg == "--graph-redo") && hasValue) {
            int count = 0;
            if (!parsePositive(argv[++i], count)) {
                kast::log::error(std::format("Invalid {} '{}', expected a positive count", arg, argv[i]));
                return std::nullopt;
            }
            for (int n = 0; n < count; ++n)
                options.graphSteps.emplace_back(arg, "");
        } else if (arg == "--graph-save" || arg == "--graph-view-code") {
            options.graphSteps.emplace_back(arg, "");
        } else if (arg == "--graph-preview" && hasValue) {
            options.graphSteps.emplace_back(arg, argv[++i]);
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
        } else if (arg == "--gui-set-attr" && hasValue) {
            const std::string text = argv[++i];
            const size_t equals = text.find('=');
            if (equals == 0 || equals == std::string::npos) {
                kast::log::error(std::format("Invalid --gui-set-attr '{}', expected name=value", text));
                return std::nullopt;
            }
            options.guiSetAttr.emplace(text.substr(0, equals), text.substr(equals + 1));
        } else if (arg == "--gui-set-prop" && hasValue) {
            const std::string text = argv[++i];
            const size_t equals = text.find('=');
            if (equals == 0 || equals == std::string::npos) {
                kast::log::error(std::format("Invalid --gui-set-prop '{}', expected name=value", text));
                return std::nullopt;
            }
            options.guiSetProp.emplace(text.substr(0, equals), text.substr(equals + 1));
        } else if (arg == "--gui-rich-wrap" && hasValue) { // tag[=value],start,end
            const std::string text = argv[++i];
            const size_t second = text.rfind(',');
            const size_t first = second == std::string::npos || second == 0 ? std::string::npos : text.rfind(',', second - 1);
            Options::GuiRichWrap wrap;
            const auto offset = [&](size_t from, size_t to, size_t& out) {
                const auto [end, error] = std::from_chars(text.data() + from, text.data() + to, out);
                return error == std::errc() && end == text.data() + to && from < to;
            };
            if (first == std::string::npos || first == 0 || !offset(first + 1, second, wrap.start) ||
                !offset(second + 1, text.size(), wrap.end)) {
                kast::log::error(std::format("Invalid --gui-rich-wrap '{}', expected tag[=value],start,end", text));
                return std::nullopt;
            }
            const std::string tag = text.substr(0, first);
            const size_t equals = tag.find('=');
            wrap.tag = tag.substr(0, equals);
            wrap.value = equals == std::string::npos ? "" : tag.substr(equals + 1);
            options.guiRichWrap = std::move(wrap);
        } else if (arg == "--gui-undo" && hasValue && parsePositive(argv[i + 1], options.guiUndo)) {
            ++i;
        } else if (arg == "--gui-redo" && hasValue && parsePositive(argv[i + 1], options.guiRedo)) {
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
        } else if (arg == "--zone-section" && hasValue) {
            for (const auto part : std::views::split(std::string_view(argv[++i]), ','))
                options.zoneSections.emplace_back(std::string_view(part.begin(), part.end()));
        } else if (arg == "--zone-tool" && hasValue) {
            if (!(options.zoneTool = ZonePanel::toolNamed(argv[++i]))) {
                kast::log::error(std::format("Unknown --zone-tool '{}'", argv[i]));
                return std::nullopt;
            }
        } else if (arg == "--zone-select" && hasValue) {
            options.zoneSelect = argv[++i];
        } else if (arg == "--bake") {
            options.bake = true;
        } else if (arg == "--mapbake" && hasValue) {
            options.mapbake = argv[++i];
        } else if (const std::optional<bool> zoneValue = zone_cli::takesValue(arg); zoneValue && (!*zoneValue || hasValue)) {
            options.zoneOps.push_back({std::string(arg), *zoneValue ? argv[++i] : ""});
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--no-recycle") {
            options.noRecycle = true;
        } else if (arg == "--sculpt-loop") {
            options.sculptLoop = true;
        } else if (arg == "--file-save") {
            options.fileSave = true;
        } else if (arg == "--bake-probes") {
            options.bakeProbes = true;
        } else if (arg == "--bake-lightmaps") {
            options.bakeLightmaps = true;
        } else if (arg == "--bake-voxelgi") {
            options.bakeVoxelGi = true;
        } else if (arg == "--bake-lightmaps-async") {
            options.bakeLightmapsAsync = true;
        } else if (arg == "--cancel-lightmaps" && hasValue && parsePositive(argv[i + 1], options.cancelLightmaps)) {
            ++i;
        } else if (arg == "--scene-undo" && hasValue && parsePositive(argv[i + 1], options.undo)) {
            ++i;
        } else if (arg == "--scene-redo" && hasValue && parsePositive(argv[i + 1], options.redo)) {
            ++i;
        } else if (arg == "--focus" && hasValue) {
            if (!(options.focus = zone_cli::parseFocus(argv[++i]))) {
                kast::log::error(std::format("Invalid --focus '{}', expected x,z,distance", argv[i]));
                return std::nullopt;
            }
        } else if (arg == "--prompt-remove-tile" && hasValue) {
            if (!(options.promptRemoveTile = zone_cli::parseTile(argv[++i]))) {
                kast::log::error(std::format("Invalid --prompt-remove-tile '{}', expected tx,tz", argv[i]));
                return std::nullopt;
            }
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
        } else if ((arg == "--create-prefab" || arg == "--drop-prefab" || arg == "--drop-prefab-view" || arg == "--set-field" ||
                    arg == "--prefab-revert" || arg == "--prefab-apply" || arg == "--prefab-unpack" || arg == "--edit-prefab") &&
                   hasValue) {
            options.prefabSteps.emplace_back(arg, argv[++i]);
        } else if (arg == "--prefab-save" || arg == "--prefab-back") {
            options.prefabSteps.emplace_back(arg, "");
        } else if (arg == "--assets-delete" && hasValue) {
            options.assetsDelete = argv[++i];
        } else if (arg == "--inspector-apply-menu" && hasValue) {
            options.inspectorApplyMenu = argv[++i];
        } else if (arg == "--prefab-back-prompt") {
            options.prefabBackPrompt = true;
        } else if (arg == "--log-fps") {
            options.logFps = true;
        } else if (arg == "--log-sdfgi") {
            kast::setSdfgiLogging(true); // each frame's SDFGI rebuilt cells and probes (sdfgi.h)
        } else if (arg == "--no-vsync") {
            options.noVsync = true;
        } else if (arg == "--no-instancing") {
            options.noInstancing = true;
        } else if (arg == "--no-culling") {
            options.noCulling = true;
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
        } else if (arg == "--fixed-dt" && hasValue) { // deterministic screenshots of anything that moves
            const std::string_view value = argv[++i];
            if (!parsePositive(value, options.fixedDt)) {
                kast::log::error(std::format("Invalid --fixed-dt '{}', expected a positive number of seconds. {}", value,
                                             kUsage));
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
            kast::log::error(std::format("Invalid argument '{}'. {} {}", arg, kUsage, zone_cli::kUsage));
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
                             options.openBuildSettings || options.openOnlineSettings || !options.focusPanel.empty() || !options.zoneSections.empty() || options.zoneTool || !options.zoneSelect.empty() || options.bake || !options.mapbake.empty() || options.newStateMachinePrompt ||
                             options.assetsMenu || options.assetsCreate || !options.assetsSelect.empty() ||
                             !options.materialSets.empty() || options.materialUndo || options.materialRedo ||
                             !options.openGui.empty() ||
                             !options.guiSelect.empty() || options.guiPreviewSize || options.guiPick ||
                             options.guiAdd || options.guiSetAttr || options.guiSetProp || options.guiRichWrap || options.guiSave || !options.guiClick.empty() || options.openPreferences ||
                             !options.openGraph.empty() || !options.previewModel.empty() ||
                             !options.openExternal.empty() || !options.dropModel.empty() || options.logFps ||
                             !options.prefabSteps.empty() || !options.assetsDelete.empty() || !options.inspectorApplyMenu.empty() ||
                             options.prefabBackPrompt ||
                             options.noVsync || options.noInstancing || options.noCulling || !options.worldAddress.empty() || options.onlineLoginEnv ||
                             options.onlineCompose || !options.zoneOps.empty() || options.headless || options.noRecycle ||
                             options.sculptLoop || options.focus || options.promptRemoveTile || options.fileSave ||
                             options.bakeProbes || options.bakeLightmaps || options.bakeLightmapsAsync ||
                             options.cancelLightmaps || options.bakeVoxelGi || options.undo || options.redo;
    if (!options.editor() && editorFlags) {
        kast::log::error("--select, --play, --select-network-entity, --add-component-popup, --new-component[-prompt], "
                         "--new-state-machine-prompt, --open-menu, --open-build-settings, --open-online-settings, --focus-panel, --zone-section, --zone-tool, --zone-select, --bake, --mapbake, "
                         "--assets-menu, "
                         "--assets-create, --assets-select, --open-gui, --gui-select, --gui-preview-size, --gui-pick, --gui-add, --gui-save, "
                         "--gui-click, --open-graph, --preview-model, --open-preferences, --open-external, --world-address, --online-login-env, --online-compose, the prefab "
                         "steps, --scene-undo, --scene-redo, --assets-delete, --inspector-apply-menu, --file-save, --bake-probes, --bake-lightmaps[-async], --cancel-lightmaps, --bake-voxelgi and the zone flags require "
                         "--project <dir> or --scene <file>");
        return std::nullopt;
    }
    if (!options.saveScene.empty() && options.project.empty() && options.scene.empty()) {
        kast::log::error("--save-scene requires --scene <file> or --project <dir>");
        return std::nullopt;
    }
    if ((!options.guiSelect.empty() || options.guiPick || options.guiAdd || options.guiSetAttr || options.guiSetProp ||
         options.guiRichWrap || options.guiUndo || options.guiRedo || options.guiSave) && options.openGui.empty()) {
        kast::log::error("--gui-select, --gui-pick, --gui-add, --gui-set-attr, --gui-set-prop, --gui-rich-wrap, --gui-undo, "
                         "--gui-redo and --gui-save require --open-gui <file.rml>");
        return std::nullopt;
    }
    if (!options.graphSteps.empty() && options.openGraph.empty()) {
        kast::log::error("--graph-* flags require --open-graph <file.kshadergraph>");
        return std::nullopt;
    }
    if ((!options.previewClip.empty() || options.previewLod != 0) && options.previewModel.empty()) {
        kast::log::error("--preview-clip and --preview-lod require --preview-model <path>");
        return std::nullopt;
    }
    if (!options.guiClick.empty() && !options.play) {
        kast::log::error("--gui-click requires --play");
        return std::nullopt;
    }
    if (!options.simKeys.empty() && !options.play && options.openGraph.empty()) {
        kast::log::error("--sim-key requires --play or --open-graph");
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
    ImGui::DockBuilderDockWindow(ZonePanel::kWindow, left);
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
    ImGui::DockBuilderDockWindow(ShaderGraphEditor::kCanvasWindow, center);
    ImGui::DockBuilderDockWindow(ShaderGraphEditor::kInspectorWindow, right);
    ImGui::DockBuilderDockWindow(ShaderGraphEditor::kPreviewWindow, left);
    ImGui::DockBuilderDockWindow(ModelPreview::kWindow, center);
    ImGui::DockBuilderFinish(dockspace);
}

// The root window that has keyboard focus (a docked panel, not its child windows); empty for none, menus and popups.
std::string focusedWindow()
{
    const ImGuiWindow* window = ImGui::GetCurrentContext()->NavWindow;
    if (!window || !window->RootWindow || (window->RootWindow->Flags & ImGuiWindowFlags_Popup) ||
        std::string_view(window->RootWindow->Name).starts_with("##"))
        return {};
    return window->RootWindow->Name;
}

// The document Edit > Undo / Redo and the shortcuts act on, by the focused window (menus and popups: the one before).
// The GUI Builder's panels edit the open GUI, the Shader Graph panels the open graph, the Zone tab the open map, the Inspector showing a material that .mat;
// every other window edits the scene (or the open prefab).
UndoTarget focusedUndoTarget(const std::string& window, Document& doc, GuiBuilder& guiBuilder, ShaderGraphEditor& graphs,
                             ZoneSession& zone, Panels& panels)
{
    if (window.ends_with(ShaderGraphEditor::kCanvasWindow) || window.ends_with(ShaderGraphEditor::kInspectorWindow))
        return {.history = &graphs.history(),
                .undo = [&graphs] { graphs.undo(); },
                .redo = [&graphs] { graphs.redo(); },
                .blocked = doc.play != PlayState::Stopped};
    if (UndoStack* materials = panels.materialHistory(); materials && window == "Inspector")
        return {.history = materials, .undo = [materials] { materials->undo(); }, .redo = [materials] { materials->redo(); }};
    constexpr const char* kGuiWindows[] = {GuiBuilder::kHierarchyWindow, GuiBuilder::kInspectorWindow,
                                           GuiBuilder::kStyleWindow, GuiBuilder::kPreviewWindow};
    if (std::ranges::any_of(kGuiWindows, [&](const char* id) { return window.ends_with(id); }))
        return {.history = &guiBuilder.history(),
                .undo = [&guiBuilder] { guiBuilder.undo(); },
                .redo = [&guiBuilder] { guiBuilder.redo(); },
                .blocked = doc.play != PlayState::Stopped};
    if (window == ZonePanel::kWindow) {
        zone::ZoneDoc* z = zone.zone();
        if (!z)
            return {}; // no map: the scene's history stays out of the way
        // Blocked mid-stroke, and while the tab is read-only (Play, prefab edit mode).
        return {.history = &z->history(),
                .undo = [z] { z->undo(); },
                .redo = [z] { z->redo(); },
                .blocked = doc.play != PlayState::Stopped || doc.prefabEdit.has_value() || z->editing()};
    }
    return {.history = &doc.history,
            .undo = [&doc] { undoScene(doc); },
            .redo = [&doc] { redoScene(doc); },
            .blocked = doc.play != PlayState::Stopped};
}

// Ctrl+Z undo, Ctrl+Y and Ctrl+Shift+Z redo; ignored while a text field or drag (ImGui's own undo) or the gizmo is
// active, and while a modal or file dialog is open.
void undoShortcuts(const UndoTarget& target, const FileActions& files)
{
    if (!files.idle() || ImGui::IsAnyItemActive() || ImGuizmo::IsUsing())
        return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal) && target.canUndo())
        target.undo();
    if ((ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
         ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) &&
        target.canRedo())
        target.redo();
}

// openMenu: a main menu to open this frame (--open-menu), or empty. openPreferences: set when File > Preferences... is
// chosen. showLighting, showPostProcessing, showPhysics, showMixer, showNetwork, showOnline: those panels' visibility,
// toggled in the Window menu. undo: what the Edit menu acts on.
void drawMenuBar(FileActions& files, Document& doc, const UndoTarget& undo, const std::string& openMenu,
                 bool& openPreferences, bool& showLighting, bool& showPostProcessing, bool& showPhysics, bool& showMixer,
                 bool& showNetwork, bool& showOnline)
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
        if (ImGui::BeginMenu("Edit")) {
            // "###" keeps each item's id while its label names the step.
            const bool canUndo = undo.canUndo() && files.idle();
            const bool canRedo = undo.canRedo() && files.idle();
            const std::string undoLabel = canUndo ? "Undo " + undo.history->undoName() : "Undo";
            const std::string redoLabel = canRedo ? "Redo " + undo.history->redoName() : "Redo";
            if (ImGui::MenuItem((undoLabel + "###undo").c_str(), "Ctrl+Z", false, canUndo))
                undo.undo();
            if (ImGui::MenuItem((redoLabel + "###redo").c_str(), "Ctrl+Y", false, canRedo))
                undo.redo();
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

// The Shader Graph panels, docked beside their scene counterparts in layouts saved before they existed: Shader Graph by
// Scene, Graph Inspector by Inspector, Shader Preview by Hierarchy. time: the preview's TIME.
void drawShaderGraph(ShaderGraphEditor& graphs, float time)
{
    for (const auto& [existing, window] : {std::pair{"Scene", ShaderGraphEditor::kCanvasWindow},
                                           std::pair{"Inspector", ShaderGraphEditor::kInspectorWindow},
                                           std::pair{"Hierarchy", ShaderGraphEditor::kPreviewWindow}}) {
        if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(existing));
            settings && !ImGui::FindWindowSettingsByID(ImHashStr(window)))
            ImGui::SetNextWindowDockID(settings->DockId, ImGuiCond_FirstUseEver);
        if (window == ShaderGraphEditor::kCanvasWindow)
            graphs.drawCanvas();
        else if (window == ShaderGraphEditor::kInspectorWindow)
            graphs.drawInspector();
        else
            graphs.drawPreview(time);
    }
    graphs.drawModals();
}

// "N.pin" (a graph node id and one of its pins); nullopt if it doesn't parse.
std::optional<kast::GraphPin> parseGraphPin(std::string_view text)
{
    const size_t dot = text.find('.');
    kast::GraphPin pin;
    if (dot == std::string_view::npos || dot + 1 == text.size() || !parsePositive(text.substr(0, dot), pin.node))
        return std::nullopt;
    pin.pin = std::string(text.substr(dot + 1));
    return pin;
}

// Comma-separated numbers ("1,2.5,3"); false if any doesn't parse.
bool parseNumbers(std::string_view text, std::vector<float>& out)
{
    for (const auto part : std::views::split(text, ',')) {
        const std::string_view value(part.begin(), part.end());
        float number = 0.0f;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (ec != std::errc{} || ptr != value.data() + value.size())
            return false;
        out.push_back(number);
    }
    return true;
}

// One --open-graph / --graph-* step, as the Shader Graph panels do it. selected: the --graph-select nodes (--graph-paste
// copies them). Logs what it did, or why not (false).
bool runGraphStep(ShaderGraphEditor& graphs, const std::filesystem::path& project, const std::string& flag,
                  const std::string& value, std::vector<int>& selected)
{
    ShaderGraphDoc& doc = graphs.doc();
    std::string error = "expected " + std::string(flag == "--graph-add"       ? "type[,x,y]"
                                                  : flag == "--graph-link"    ? "N.pin,M.pin"
                                                  : flag == "--graph-set"     ? "N.setting=json"
                                                  : flag == "--graph-comment" ? "title,x,y,w,h"
                                                  : flag == "--graph-paste"   ? "x,y"
                                                                              : "a value");
    bool ok = false;
    const size_t comma = value.find(',');
    if (flag == "--open-graph") {
        std::filesystem::path file = std::filesystem::path(reinterpret_cast<const char8_t*>(value.c_str()));
        graphs.open(std::filesystem::absolute(file.is_relative() && !project.empty() ? project / file : file).lexically_normal());
        return true;
    } else if (flag == "--graph-add") {
        std::vector<float> at;
        if (comma == std::string::npos || (parseNumbers(std::string_view(value).substr(comma + 1), at) && at.size() == 2)) {
            const std::array<float, 2> position = at.empty() ? graphs.viewCenter() : std::array{at[0], at[1]};
            const int id = doc.addNode(value.substr(0, comma), position, {}, &error);
            if ((ok = id != 0))
                kast::log::info(std::format("--graph-add {}: node {}", value, id));
        }
    } else if (flag == "--graph-link") {
        const auto from = parseGraphPin(std::string_view(value).substr(0, comma));
        const auto to = comma == std::string::npos ? std::nullopt : parseGraphPin(std::string_view(value).substr(comma + 1));
        if (from && to && !(ok = doc.link(*from, *to, error))) {
            graphs.showRefusedLink(*from, *to, error); // the red link and why, as a refused drag shows them
            kast::log::warn(std::format("--graph-link {} refused: {}", value, error));
            return true;
        }
    } else if (flag == "--graph-unlink") {
        const auto to = parseGraphPin(value);
        error = "no link into that input";
        ok = to && doc.unlink(*to);
    } else if (flag == "--graph-select" || flag == "--graph-delete") {
        std::vector<float> ids;
        error = "expected N[,M...]";
        if ((ok = parseNumbers(value, ids))) {
            selected.clear();
            for (float id : ids)
                selected.push_back(static_cast<int>(id));
            if (flag == "--graph-select")
                graphs.select(selected);
            else
                ok = doc.remove(selected);
        }
    } else if (flag == "--graph-set") {
        const size_t dot = value.find('.'), equals = value.find('=');
        int node = 0;
        const nlohmann::ordered_json json = equals == std::string::npos ? nlohmann::ordered_json()
                                            : nlohmann::ordered_json::parse(value.substr(equals + 1), nullptr, false);
        if (dot < equals && equals != std::string::npos && parsePositive(std::string_view(value).substr(0, dot), node) &&
            !json.is_discarded())
            ok = doc.setSetting(node, value.substr(dot + 1, equals - dot - 1), json, error);
    } else if (flag == "--graph-comment") {
        std::vector<float> rect;
        if (comma != std::string::npos && parseNumbers(std::string_view(value).substr(comma + 1), rect) && rect.size() == 4) {
            doc.addComment({value.substr(0, comma), {rect[0], rect[1], rect[2], rect[3]}});
            ok = true;
        }
    } else if (flag == "--graph-paste") {
        std::vector<float> at;
        if (parseNumbers(value, at) && at.size() == 2) {
            const std::vector<int> pasted = doc.paste(doc.copy(selected), {at[0], at[1]}, error);
            if ((ok = !pasted.empty())) {
                graphs.select(pasted);
                std::string ids;
                for (int id : pasted)
                    ids += (ids.empty() ? "" : ",") + std::to_string(id);
                kast::log::info(std::format("--graph-paste {}: nodes {}", value, ids));
            }
        }
    } else if (flag == "--graph-add-menu") {
        const auto pin = value == "/" ? std::nullopt : parseGraphPin(value);
        error = "expected N.pin or /";
        if ((ok = value == "/" || pin))
            graphs.openAddMenu(pin);
    } else if (flag == "--graph-undo" || flag == "--graph-redo") {
        error = "nothing to " + flag.substr(8);
        ok = flag == "--graph-undo" ? graphs.history().canUndo() : graphs.history().canRedo();
        if (ok)
            flag == "--graph-undo" ? graphs.undo() : graphs.redo();
    } else if (flag == "--graph-save") {
        error = "the write failed";
        ok = graphs.save();
    } else if (flag == "--graph-view-code") {
        graphs.showGeneratedCode();
        ok = true;
    } else if (flag == "--graph-preview") {
        error = "expected sphere, cube or plane";
        constexpr std::pair<std::string_view, kast::Primitive> kMeshes[] = {
            {"sphere", kast::Primitive::Sphere}, {"cube", kast::Primitive::Cube}, {"plane", kast::Primitive::Plane}};
        const auto mesh = std::ranges::find(kMeshes, std::string_view(value), &std::pair<std::string_view, kast::Primitive>::first);
        ok = mesh != std::end(kMeshes);
        if (ok)
            graphs.preview().mesh = mesh->second;
    }
    if (!ok)
        kast::log::error(std::format("{} {}: {}", flag, value, error));
    return ok;
}

// "ID[:Component[.field] | :#local][@hop]" (--prefab-revert / --prefab-apply): whole-instance, deleted-child, component
// or field op; component "Name" is the entity name. nullopt if it doesn't parse.
std::optional<PrefabOp> parsePrefabOp(std::string_view spec, bool apply)
{
    using Kind = PrefabOp::Kind;
    PrefabOp op;
    if (const size_t at = spec.rfind('@'); at != std::string_view::npos) {
        int hop = 0;
        const std::string_view text = spec.substr(at + 1);
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), hop);
        if (ec != std::errc{} || ptr != text.data() + text.size() || hop < 0)
            return std::nullopt;
        op.hop = static_cast<size_t>(hop);
        spec = spec.substr(0, at);
    }
    const size_t colon = spec.find(':');
    if (!parsePositive(spec.substr(0, colon), op.entity))
        return std::nullopt;
    const std::string_view target = colon == std::string_view::npos ? "" : spec.substr(colon + 1);
    if (target.empty()) {
        op.kind = apply ? Kind::ApplyAll : Kind::RevertAll;
    } else if (target.front() == '#') {
        if (!parsePositive(target.substr(1), op.local))
            return std::nullopt;
        op.kind = apply ? Kind::ApplyDeleted : Kind::RevertDeleted;
    } else if (const size_t dot = target.find('.'); dot != std::string_view::npos || target == "Name") {
        op.kind = apply ? Kind::ApplyField : Kind::RevertField;
        op.component = std::string(target.substr(0, dot));
        op.field = dot == std::string_view::npos ? "" : std::string(target.substr(dot + 1));
    } else {
        op.kind = apply ? Kind::ApplyComponent : Kind::RevertComponent;
        op.component = std::string(target);
    }
    return op;
}

// One --create-prefab ... --prefab-back step, as the Hierarchy, Scene view, Inspector and Assets panel would do it.
// False (logged) if it fails.
bool runPrefabStep(Document& doc, const std::string& flag, const std::string& value)
{
    std::string error;
    const auto lastNumber = [](const std::string& text, std::string& head, float& number) {
        const size_t comma = text.rfind(',');
        if (comma == std::string::npos)
            return false;
        const char* begin = text.data() + comma + 1;
        const auto [ptr, ec] = std::from_chars(begin, text.data() + text.size(), number);
        head = text.substr(0, comma);
        return ec == std::errc{} && ptr == text.data() + text.size();
    };
    bool ok = false;
    if (flag == "--create-prefab") {
        std::string path;
        ok = createPrefabFrom(doc, doc.selected, value == "/" ? "" : value, path, error);
    } else if (flag == "--drop-prefab") {
        std::string path = value;
        entt::entity parent = entt::null;
        if (const size_t comma = value.rfind(','); comma != std::string::npos) {
            kast::EntityId id = 0;
            if (!parsePositive(std::string_view(value).substr(comma + 1), id) ||
                (parent = kast::findById(doc.scene, id)) == entt::null) {
                kast::log::error(std::format("--drop-prefab {}: no parent entity with that id", value));
                return false;
            }
            path = value.substr(0, comma);
        }
        ok = dropPrefab(doc, path, parent, error) != entt::null;
    } else if (flag == "--drop-prefab-view") {
        std::string rest, path;
        float u = 0.0f, v = 0.0f;
        if (!lastNumber(value, rest, v) || !lastNumber(rest, path, u)) {
            kast::log::error(std::format("Invalid --drop-prefab-view '{}', expected path,u,v", value));
            return false;
        }
        // Headless: a 16:9 Scene view of the editor camera, D3D depth (what the editor renders with).
        ok = dropPrefabOnView(doc, path, editorCameraRay(doc, u, v, 16.0f / 9.0f, false), error) != entt::null;
    } else if (flag == "--set-field") {
        const size_t first = value.find(','), second = first == std::string::npos ? first : value.find(',', first + 1);
        kast::EntityId id = 0;
        const std::string target = second == std::string::npos ? "" : value.substr(first + 1, second - first - 1);
        const size_t dot = target.find('.');
        const entt::entity entity = second != std::string::npos && parsePositive(std::string_view(value).substr(0, first), id)
                                        ? kast::findById(doc.scene, id)
                                        : entt::null;
        nlohmann::ordered_json json = second == std::string::npos ? nlohmann::ordered_json()
                                                                  : nlohmann::ordered_json::parse(value.substr(second + 1), nullptr, false);
        if (entity == entt::null || json.is_discarded() || (dot == std::string::npos && target != "Name")) {
            kast::log::error(std::format("Invalid --set-field '{}', expected ID,Component.field,json (or ID,Name,\"text\")", value));
            return false;
        }
        entt::registry& registry = doc.scene.registry();
        beginSceneEdit(doc, std::format("Set {}", target), {.entities = {id}}); // as an Inspector edit
        if (target == "Name") {
            ok = json.is_string();
            if (ok)
                registry.get<kast::Name>(entity).value = json.get<std::string>();
            else
                error = "the name must be a JSON string";
        } else if (const kast::ComponentType type = kast::findAnyComponentType(target.substr(0, dot));
                   type && kast::hasComponent(registry, entity, type)) {
            nlohmann::ordered_json component = kast::componentToJson(registry, entity, type);
            component[target.substr(dot + 1)] = json;
            ok = kast::setComponentJson(registry, entity, type, component, error);
        } else {
            error = std::format("entity {} has no \"{}\"", id, target.substr(0, dot));
        }
        if (ok)
            markDirty(doc);
        endSceneEdit(doc);
    } else if (flag == "--prefab-revert" || flag == "--prefab-apply") {
        const std::optional<PrefabOp> op = parsePrefabOp(value, flag == "--prefab-apply");
        if (!op) {
            kast::log::error(std::format("Invalid {} '{}', expected ID[:Component[.field] | :#local][@hop]", flag, value));
            return false;
        }
        ok = runPrefabOp(doc, *op, error);
    } else if (flag == "--prefab-unpack") {
        PrefabOp op{PrefabOp::Kind::Unpack};
        ok = parsePositive(value, op.entity) && runPrefabOp(doc, op, error);
    } else if (flag == "--edit-prefab") {
        ok = openPrefab(doc, value, error);
    } else if (flag == "--prefab-save") {
        error = "no prefab is open (--edit-prefab)";
        ok = doc.prefabEdit && saveFile(doc, doc.path, error);
    } else if (flag == "--prefab-back") {
        error = "no prefab is open (--edit-prefab)";
        ok = doc.prefabEdit.has_value();
        closePrefab(doc);
    }
    if (!ok)
        kast::log::error(std::format("{} {}: {}", flag, value, error));
    return ok;
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
    // .kshader materials compile with shaderc here (not in the player), on worker threads; a --screenshot run compiles
    // each one when first drawn instead, so the shot is the same every run (--screenshot-on-log runs, e.g. the hot
    // reload test, keep the worker threads).
    kast::shaders().setCompiler(compileShader, shaderToolsVersion());
    kast::shaders().setAsync(options->screenshot.empty() || options->screenshotOnLog);
    // Models import with Assimp here (not in the player), once: <project>/.kast/model_cache holds the result.
    kast::models().setImporter(importModel, modelImporterVersion());
    // PNG / JPG textures compress with texturec here (not in the player) into <project>/.kast/texture_cache, on worker
    // threads while the uncompressed image draws (a --screenshot run cooks each one when first drawn, as shaders).
    kast::textureCooks().setCooker(cookTextureWithTexturec, texturecVersion());
    kast::textureCooks().setAsync(options->screenshot.empty() || options->screenshotOnLog);
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
    if (options->select != 0 && !selectById(doc, options->select)) {
        kast::log::error(std::format("--select {}: no entity with that id", options->select));
        return 1;
    }
    for (const auto& [flag, value] : options->prefabSteps)
        if (!runPrefabStep(doc, flag, value))
            return 1;
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
    if (!options->newComponent.empty()) { // before --play, which blocks it
        if (std::string error; !newLuaComponent(doc, doc.selected, options->newComponent, error))
            kast::log::error(std::format("--new-component {}: {}", options->newComponent, error));
    }
    if (!options->dropModel.empty())
        createModelEntity(doc, options->dropModel, entt::null);
    for (int i = 0; i < options->undo; ++i)
        undoScene(doc);
    for (int i = 0; i < options->redo; ++i)
        redoScene(doc);
    refreshStateMachines(doc); // the Inspector and State Machines panel list them from the first frame
    const std::filesystem::path settingsFile =
        options->settingsFile.empty() ? defaultEditorSettingsFile() : std::filesystem::absolute(options->settingsFile);
    ZoneSession zone; // the scene's zone: the Scene view shows it, the Zone tab edits it
    zone.setRecycle(!options->noRecycle);
    if (!options->zoneOps.empty() || options->headless) { // the zone edit flags (zone_cli.h)
        if (!doc.project) {
            kast::log::error("The zone flags need a project: --project <dir> or a --scene inside one");
            return 1;
        }
        zone.openProject(doc.project->root);
        if (!zone_cli::run(zone, options->zoneOps))
            return 1;
        if (options->headless)
            return options->bake ? zone_cli::bakeHeadless(zone, settingsFile, options->mapbake) : 0;
        // The window shows the map they opened: the scene's Terrain entity points at it (added if there is none).
        const auto terrains = doc.scene.registry().view<kast::Terrain>();
        if (const std::optional<int> map = zone.loadedMap();
            map && (terrains.empty() || doc.scene.registry().get<kast::Terrain>(terrains.front()).map != *map))
            ZonePanel::setTerrainMap(doc, *map);
        if (zone.zone())
            zone_cli::frameZone(doc.camera, *zone.zone());
    }
    // The Online panel's login goes to each Play session and comes back at Stop (refresh tokens rotate: one holder).
    OnlineModel online(settingsFile);
    doc.onPlayStarted = [&online, &zone](kast::LuaSystems& lua) {
        online.lendSession(lua.auth());
        if (zone.dirty())
            kast::log::warn(std::format("Zone: maps/{} has unsaved changes; Play uses the saved map files",
                                        zone.zone()->folder));
    };
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
    zone.update(doc); // before --play, which freezes it: the Zone tab shows the zone during that Play
    if (options->focus) {
        const auto [x, z, distance] = *options->focus;
        doc.camera.pivot = {x, zone_cli::groundY(zone.zone(), x, z), z};
        doc.camera.distance = distance;
        doc.camera.pitch = -50.0f;
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
    ZoneRender zoneRender;
    ZonePanel zonePanel(settingsFile);
    if (!options->mapbake.empty())
        zonePanel.setMapbake(options->mapbake);
    bool bakeStarted = false; // --bake: once (the frame count holds while it runs)
    int bakeFrames = 0;       // --bake: frames drawn while kast_mapbake ran
    // A dirty zone is part of the unsaved-changes prompt, and File > Save / Ctrl+S writes it after the scene.
    files.extra = {.unsaved = [&] { return zone.dirty() ? "maps/" + zone.zone()->folder : std::string(); },
                   .save = [&] { return zonePanel.save(zone); },
                   .discard = [&] { zone.reload(); }};
    // The open zone in the Scene view, edit mode only: Play streams the saved map from disk (TerrainStreamer), and a
    // prefab is edited on its own.
    const auto showZone = [&] { return doc.play == PlayState::Stopped && !doc.prefabEdit; };
    viewport.overlay = [&](const float* viewProj, float x, float y, float width, float height) {
        zoneRender.drawOverlays(ImGui::GetWindowDrawList(), viewProj, x, y, width, height);
        if (showZone())
            zonePanel.drawSceneOverlay(zone, ImGui::GetWindowDrawList(), viewProj, x, y, width, height);
    };
    // An active Zone tab tool gets the Scene view's left mouse instead of picking and the gizmo.
    viewport.input = [&](const SceneViewport::Input& input) { return showZone() && zonePanel.sceneInput(zone, input); };
    GuiBuilder guiBuilder(kGuiPreviewView);
    ShaderGraphEditor graphs(kShaderPreviewView);
    ModelPreview modelPreview(kModelPreviewView);
    UiScale uiScale(options->uiScale);
    FrameLimit frames(options->frames, options->screenshot);
    if (options->screenshotOnLog)
        frames.endOnLog(options->screenshotOnLog->first, options->screenshotOnLog->second);
    size_t simLogSeen = 0; // --sim-on-log: log lines already searched
    std::optional<int> simStart; // the frame --sim-mouse / --sim-key frames count from; unset until --sim-on-log's line
    if (options->simOnLog.empty())
        simStart = 0;
    float componentPollSeconds = 0.0f;
    std::string undoWindow; // the last focused window that isn't a menu or popup: picks the Edit menu's document
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
        for (kast::Renderer* renderer : {&viewport.renderer(), &game.renderer()}) {
            renderer->setInstancing(!options->noInstancing);
            renderer->setCulling(!options->noCulling);
        }
        kast::gui::init();
        if (!options->openGui.empty()) {
            std::filesystem::path rml = std::filesystem::path(reinterpret_cast<const char8_t*>(options->openGui.c_str()));
            if (rml.is_relative() && doc.project)
                rml = doc.project->root / rml;
            guiBuilder.open(std::filesystem::absolute(rml).lexically_normal());
            if (!options->guiSelect.empty() && !guiBuilder.selectById(options->guiSelect))
                kast::log::error(std::format("--gui-select {}: no element with that id", options->guiSelect));
        }
        if (!options->openGraph.empty()) {
            std::vector<int> none;
            runGraphStep(graphs, doc.project ? doc.project->root : std::filesystem::path(), "--open-graph",
                         options->openGraph, none);
        }
        if (!options->previewModel.empty()) {
            modelPreview.open(options->previewModel);
            modelPreview.selectClip(options->previewClip);
            modelPreview.selectLod(options->previewLod);
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
    panels.probeStatus = [&](const kast::ReflectionProbe& probe) { return viewport.renderer().probeStatus(probe); };
    LightmapJob lightmapJob; // Bake Lightmaps on a worker thread
    panels.lightmapStatus = [&](const kast::Scene& scene) { return viewport.renderer().lightmapStatus(scene); };
    panels.lightmapProgress = [&] { return lightmapJob.progress(); };
    VoxelGiJob voxelGiJob; // Bake VoxelGI on a worker thread
    panels.voxelGiStatus = [&](const kast::Scene& scene, entt::entity volume) { return viewport.renderer().voxelGiStatus(scene, volume); };
    panels.voxelGiProgress = [&] { return voxelGiJob.progress(); };
    hooks.frame = [&] {
        const uint64_t frameStart = SDL_GetPerformanceCounter();
        // Before this frame submits anything: a bake runs frames of its own on the Scene view's views.
        if (const std::vector<entt::entity> probes = panels.takeBakeRequests(); !probes.empty())
            viewport.bakeProbes(doc, probes);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        online.poll(doc.project ? &*doc.project : nullptr, onlinePanel.visible());
        applyWorldOverride();
        if (playAfterLogin && !online.loggingIn()) {
            playAfterLogin = false;
            startPlay(doc);
        }
        updatePlay(doc, options->fixedDt > 0.0f ? options->fixedDt : ImGui::GetIO().DeltaTime);
        for (kast::Renderer* renderer : {&viewport.renderer(), &game.renderer()})
            renderer->setTime(doc.shaderClock.seconds());
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
            graphs.poll();
        }

        bool openPreferences = options->openPreferences && frames.frame() == kAddComponentPopupFrame;
        if (std::string window = focusedWindow(); !window.empty())
            undoWindow = std::move(window);
        const UndoTarget undo = focusedUndoTarget(undoWindow, doc, guiBuilder, graphs, zone, panels);
        undoShortcuts(undo, files);
        // With --open-gui or --focus-panel: after the panel took the focus (which closes menus) and the --gui-* edits.
        const int openMenuFrame =
            kAddComponentPopupFrame +
            (options->openGui.empty() && options->openGraph.empty() && options->focusPanel.empty() ? 0 : 10);
        drawMenuBar(files, doc, undo, frames.frame() == openMenuFrame ? options->openMenu : std::string(),
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
        // With --open-graph: after the graph panels took the focus (Shader Preview, Graph Inspector, canvas).
        if (!options->focusPanel.empty() && frames.frame() == kAddComponentPopupFrame + (options->openGraph.empty() ? 0 : 3)) {
            // Applied when that window is submitted below. Builder panels go by their "###" id.
            const std::string id = "###" + options->focusPanel;
            ImGui::SetWindowFocus(ImGui::FindWindowByName(id.c_str()) ? id.c_str() : options->focusPanel.c_str());
        }
        if (!options->zoneSections.empty() && frames.frame() == kAddComponentPopupFrame)
            zonePanel.showSections(options->zoneSections);
        if (options->zoneTool && frames.frame() == kAddComponentPopupFrame)
            zonePanel.setTool(*options->zoneTool);
        if (!options->zoneSelect.empty() && frames.frame() == kAddComponentPopupFrame)
            zonePanel.select(zone, options->zoneSelect);
        if (options->bake && frames.frame() == kAddComponentPopupFrame && !std::exchange(bakeStarted, true))
            zonePanel.startBake(zone);
        if (options->promptRemoveTile && frames.frame() == kAddComponentPopupFrame + 2) // after --focus-panel
            zonePanel.askRemoveTile({(*options->promptRemoveTile)[0], (*options->promptRemoveTile)[1]});
        if (options->bakeProbes && frames.frame() == kAddComponentPopupFrame - 2) // baked next frame, saved after
            panels.requestBakeAll(doc);
        if ((options->bakeLightmaps || options->bakeLightmapsAsync) && frames.frame() == kAddComponentPopupFrame - 2) {
            std::string error;
            if (!lightmapJob.start(doc, error))
                kast::log::error(std::format("Bake Lightmaps: {}", error));
            else if (options->bakeLightmaps)
                lightmapJob.wait(); // done (and written by poll below) before --file-save's frame
        }
        if (options->cancelLightmaps > 0 && frames.frame() == options->cancelLightmaps)
            lightmapJob.cancel();
        if (panels.takeLightmapBakeRequest()) {
            if (std::string error; !lightmapJob.start(doc, error))
                kast::log::error(std::format("Bake Lightmaps: {}", error));
        }
        if (panels.takeLightmapCancelRequest())
            lightmapJob.cancel();
        lightmapJob.poll(doc, static_cast<uint64_t>(frames.frame()));
        if (options->bakeVoxelGi && frames.frame() == kAddComponentPopupFrame - 2) {
            std::string error;
            if (!voxelGiJob.start(doc, {}, error))
                kast::log::error(std::format("Bake VoxelGI: {}", error));
            voxelGiJob.wait(); // done (and written by poll below) before --file-save's frame
        }
        if (std::optional<std::vector<entt::entity>> volumes = panels.takeVoxelGiBakeRequest()) {
            if (std::string error; !voxelGiJob.start(doc, std::move(*volumes), error))
                kast::log::error(std::format("Bake VoxelGI: {}", error));
        }
        if (panels.takeVoxelGiCancelRequest())
            voxelGiJob.cancel();
        voxelGiJob.poll(doc);
        if (options->fileSave && frames.frame() == kAddComponentPopupFrame)
            files.save();
        if (!options->openGui.empty() && frames.frame() == kAddComponentPopupFrame)
            guiBuilder.focusPanels(); // again: the first frames' docking undid the focus open() asked for
        if (!options->openGraph.empty() && frames.frame() == kAddComponentPopupFrame)
            graphs.focusPanels(); // again: the first frames' docking undid the focus open() asked for
        if (!options->previewModel.empty() && frames.frame() == kAddComponentPopupFrame)
            modelPreview.focus(); // the same
        if (frames.frame() == kAddComponentPopupFrame + 7) { // the canvas is laid out (its center, the add menu)
            std::vector<int> selected;
            for (const auto& [flag, value] : options->graphSteps)
                runGraphStep(graphs, doc.project ? doc.project->root : std::filesystem::path(), flag, value, selected);
            if (!options->graphSteps.empty())
                graphs.frameAll(); // the screenshot shows what they added
        }
        if (options->guiPick && frames.frame() == kAddComponentPopupFrame + 6) { // the preview has been laid out
            const auto [x, y] = *options->guiPick;
            if (guiBuilder.pick(static_cast<float>(x), static_cast<float>(y)))
                kast::log::info(std::format("--gui-pick {},{} selected '{}'", x, y, guiBuilder.selectedId()));
            else
                kast::log::error("--gui-pick: the GUI Preview isn't showing");
        }
        if (options->guiAdd && frames.frame() == kAddComponentPopupFrame + 7)
            guiBuilder.requestAdd(*options->guiAdd);
        if (options->guiSetAttr && frames.frame() == kAddComponentPopupFrame + 7) {
            const auto& [name, value] = *options->guiSetAttr;
            if (guiBuilder.setAttribute(name, value))
                kast::log::info(std::format("--gui-set-attr {}=\"{}\" on '{}'", name, value, guiBuilder.selectedId()));
            else
                kast::log::error("--gui-set-attr: no element selected (--gui-select)");
        }
        if (options->guiSetProp && frames.frame() == kAddComponentPopupFrame + 7) {
            const auto& [name, value] = *options->guiSetProp;
            if (guiBuilder.setWidgetProperty(name, value))
                kast::log::info(std::format("--gui-set-prop {}=\"{}\" on '{}'", name, value, guiBuilder.selectedId()));
            else
                kast::log::error(std::format("--gui-set-prop: the selection has no widget property '{}'", name));
        }
        if (options->guiRichWrap && frames.frame() == kAddComponentPopupFrame + 7) {
            const auto& wrap = *options->guiRichWrap;
            if (guiBuilder.wrapRichText(wrap.tag, wrap.value, wrap.start, wrap.end))
                kast::log::info(std::format("--gui-rich-wrap [{}] {}..{} on '{}'", wrap.tag, wrap.start, wrap.end,
                                            guiBuilder.selectedId()));
            else
                kast::log::error("--gui-rich-wrap: the selection is no Rich Text");
        }
        if (frames.frame() == kAddComponentPopupFrame + 8) { // after --gui-add's edit was applied
            for (int i = 0; i < options->guiUndo; ++i)
                guiBuilder.undo();
            for (int i = 0; i < options->guiRedo; ++i)
                guiBuilder.redo();
        }
        if (options->guiSave && frames.frame() == kAddComponentPopupFrame + 9)
            guiBuilder.save();
        if (!options->guiClick.empty() && frames.frame() == kAddComponentPopupFrame && !game.gui().click(options->guiClick))
            kast::log::error(std::format("--gui-click {}: no such element in a visible game GUI", options->guiClick));
        if (options->assetsMenu && frames.frame() == kAddComponentPopupFrame + 2) // after --focus-panel, which closes popups
            panels.openAssetMenu(*options->assetsMenu);
        if (!options->assetsDelete.empty() && frames.frame() == kAddComponentPopupFrame + 2)
            panels.openAssetDelete(doc, options->assetsDelete);
        if (!options->inspectorApplyMenu.empty() && frames.frame() == kAddComponentPopupFrame + 2)
            panels.openApplyMenu(options->inspectorApplyMenu);
        if (options->prefabBackPrompt && frames.frame() == kAddComponentPopupFrame + 2)
            files.backFromPrefab();
        if (options->assetsCreate && frames.frame() == kAddComponentPopupFrame)
            panels.requestAssetCreate(options->assetsCreate->first, options->assetsCreate->second);
        if (!options->assetsSelect.empty() && frames.frame() == kAddComponentPopupFrame)
            panels.selectAsset(options->assetsSelect);
        if (frames.frame() == kAddComponentPopupFrame + 1) { // the Inspector has shown --assets-select's material
            for (const auto& [field, value] : options->materialSets)
                if (std::string error; panels.editMaterial(field, value, error))
                    kast::log::info(std::format("--material-set {} = {}", field, value.dump()));
                else
                    kast::log::error(std::format("--material-set {}: {}", field, error));
            UndoStack* history = panels.materialHistory();
            for (int i = 0; history && i < options->materialUndo; ++i)
                history->undo();
            for (int i = 0; history && i < options->materialRedo; ++i)
                history->redo();
        }
        // Before the views render: a graph edit handed to the library shows in this frame's Scene and Game.
        graphs.update(doc.project ? doc.project->root : std::filesystem::path());
        panels.drawHierarchy(doc, files);
        // Before the views render: Inspector edits (a material saved to disk) show in this frame's Scene and Game.
        panels.drawInspector(doc);
        zonePanel.guardMapChange(doc, zone); // an Inspector edit of Terrain.map waits for the unsaved-zone prompt
        zone.update(doc);
        // Layouts saved before the Zone tab existed: dock it as a tab beside Hierarchy.
        if (const ImGuiWindowSettings* hierarchy = ImGui::FindWindowSettingsByID(ImHashStr("Hierarchy"));
            hierarchy && !ImGui::FindWindowSettingsByID(ImHashStr(ZonePanel::kWindow)))
            ImGui::SetNextWindowDockID(hierarchy->DockId, ImGuiCond_FirstUseEver);
        if (options->sculptLoop && showZone() && zone.zone())
            zone_cli::sculptLoop(*zone.zone(), frames.frame());
        zonePanel.draw(doc, zone); // before the zone is synced: its edits show this frame
        zoneRender.sync(showZone() ? zone.zone() : nullptr, doc.project ? doc.project->root : std::filesystem::path(),
                        viewport.renderer());
        viewport.setExtraScene(zoneRender.scene());
        // A whole 2 km+ map in view: a farther far plane, and a farther near plane for depth precision.
        doc.camera.nearPlane = zoneRender.scene() ? kZoneNearPlane : EditorCamera::kNear;
        doc.camera.farPlane = zoneRender.scene() ? kZoneFarPlane : EditorCamera::kFar;
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
        panels.drawAssets(doc, files, guiBuilder, graphs, modelPreview, externalEditor);
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
        panels.drawLighting(doc, showLighting, viewport.renderer().skyError());
        panels.drawPostProcessing(doc, showPostProcessing);
        panels.drawPhysics(doc, showPhysics);
        panels.drawMixer(doc, showMixer);
        panels.drawNetwork(doc, showNetwork);
        onlinePanel.draw(doc, online, files, options->worldAddress, showOnline);
        drawGuiBuilder(guiBuilder);
        drawShaderGraph(graphs, doc.shaderClock.seconds());
        // Layouts saved before the Model Preview existed: dock it as a tab beside Scene. Only when it draws, or the dock
        // request would go to the next window.
        if (const ImGuiWindowSettings* scene = ImGui::FindWindowSettingsByID(ImHashStr("Scene"));
            modelPreview.isOpen() && scene && !ImGui::FindWindowSettingsByID(ImHashStr(ModelPreview::kWindow)))
            ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
        modelPreview.draw(options->fixedDt > 0.0f ? options->fixedDt : ImGui::GetIO().DeltaTime);
        drawConsole(); // submitted after Assets, Events and State Machines so Console is the front tab on first run
        drawPreferences(externalEditor, online, preferencesDraft, composeDraft, openPreferences);
        externalEditor.poll();
        if (!ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) {
            endSceneGesture(doc); // the field, drag or gizmo let go: its undo step is complete
            guiBuilder.endGesture();
            graphs.endGesture(); // a drag on the canvas, or an Inspector field
        }
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
                pushSimMouse(*options->simMouse, frame, options->play ? game.imageCenter() : viewport.imageCenter(),
                             SDL_GetWindowID(mainWindow));
            for (const SimKey& key : options->simKeys)
                pushSimKey(key, frame, SDL_GetWindowID(mainWindow));
        }

        ImGui::Render();
        bgfx::touch(kImGuiView);
        darkenTextOnAccent(ImGui::GetDrawData());
        imgui_bgfx::render(ImGui::GetDrawData());
        // --bake: the frame count waits for kast_mapbake, so the screenshot shows its result (the UI keeps drawing).
        if (options->bake && zonePanel.baking()) {
            ++bakeFrames;
        } else {
            if (const int drawn = std::exchange(bakeFrames, 0); drawn > 0)
                kast::log::info(std::format("{} frames drawn while kast_mapbake ran", drawn));
            frames.endFrame();
        }
        if (options->logFps)
            fpsLog.endFrame(frameStart);
    };
    hooks.screenshotSaved = [&](const char*, bool saved) { frames.onScreenshotSaved(saved); };
    hooks.quitRequested = [&] {
        // A --frames run has no one to answer "Unsaved changes" (e.g. after --new-component): it always quits.
        return (graphs.onQuitRequested() && guiBuilder.onQuitRequested() && files.onQuitRequested()) || options->frames != 0;
    };
    hooks.shutdown = [&] {
        kast::log::info(kast::assetLoadSummary());
        guiBuilder.shutdown();
        graphs.shutdown();
        modelPreview.shutdown();
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
    if (options->bake && zonePanel.bakeExit() != 0)
        return 1;
    return frames.failed() ? 1 : 0;
}
