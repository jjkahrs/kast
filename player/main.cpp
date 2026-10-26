// kast_player: the prebuilt game client the editor's Build menu copies as <Project>.exe. Plays the project in the
// game.pak next to it from its startScene, as editor Play mode does, through the scene's Main Camera.
#include <app.h>
#include <crash_dialogs.h>
#include <files.h>
#include <gui.h>
#include <input.h>
#include <log.h>
#include <lua_components.h>
#include <lua_systems.h>
#include <pak.h>
#include <post_process.h>
#include <play.h>
#include <project.h>
#include <renderer.h>
#include <scene.h>
#include <scene_gui.h>
#include <scene_io.h>
#include <sdfgi.h>
#include <stats_overlay.h>
#include <window_display.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr bgfx::ViewId kShadowViews = 0; // the scene's shadow maps (Renderer::kShadowViews)
constexpr bgfx::ViewId kGameView = kShadowViews + kast::Renderer::kShadowViews; // then its post passes
// The scene's game GUIs (kast::gui::kViews), over the scene, then the World UI atlas (world_ui.h) in the views after theirs.
constexpr bgfx::ViewId kGuiView = kGameView + kast::PostProcess::kViews;
constexpr bgfx::ViewId kPresentView = kGuiView + kast::gui::kViews; // the readable target onto the screen, if used
constexpr uint32_t kClearColor = 0x202020ff;
constexpr int kScreenshotTimeoutFrames = 30;
constexpr const char* kPakFileName = "game.pak";
constexpr int kGuiClickFrame = 10; // --gui-click: once the GUIs have loaded and laid out
constexpr const char* kUsage =
    "Usage: <game>.exe [--frames N] [--screenshot out.png] [--gui-click id] [--sim-key frame,key]... [--sim-click frame,x,y]... [--size WxH] [--window-pos X,Y] [--user-settings file] [--fixed-dt S] [--gpu-stats] [--log-sdfgi] [--no-instancing] [--no-culling] [--no-lod] [--lod-bias F] [--force-lod N] [--no-occlusion] [--no-audio-device]";
constexpr int kGpuStatsWarmupFrames = 10; // --gpu-stats: skip startup frames (shader and target creation)

struct Options {
    int frames = 0;         // 0 = run until quit
    std::string screenshot; // captured after the last frame
    std::string guiClick;   // tests: click this GUI element id on frame kGuiClickFrame
    std::vector<std::pair<int, SDL_Scancode>> simKeys; // tests: these keys pressed on these frames (pushed SDL events)
    std::vector<std::pair<int, SDL_FPoint>> simClicks; // tests: a left click at this window position on this frame
    std::optional<std::pair<int, int>> size; // --size: a window of this size, over the project's and user's mode
    std::filesystem::path userSettings; // --user-settings: this file instead of the per-user one (tests)
    std::optional<std::pair<int, int>> windowPos; // --window-pos: window top-left
    float fixedDt = 0.0f;   // --fixed-dt: each frame advances this many seconds (0: real time), as in the editor
    bool gpuStats = false;  // log the average GPU frame time on exit
    bool noInstancing = false; // draw every mesh one by one (Renderer::setInstancing)
    bool noCulling = false;    // draw every mesh in every pass (Renderer::setCulling)
    bool noLod = false;        // draw every model at LOD0 (Renderer::setLod)
    bool noOcclusion = false;  // no Hi-Z occlusion culling (Renderer::setOcclusion)
    float lodBias = 1.0f;      // --lod-bias: Renderer::setLodBias
    int forceLod = -1;         // --force-lod: Renderer::setForcedLod (-1: by screen height)
    bool noAudioDevice = false; // tests: sound on the null backend (silent), as if there were no output device
};

bool parseInt(std::string_view text, int& out)
{
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size() && out > 0;
}

std::optional<Options> parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--frames" && hasValue) {
            const std::string_view text = argv[++i];
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), options.frames);
            if (ec != std::errc{} || ptr != text.data() + text.size() || options.frames <= 0) {
                kast::log::error(std::format("Invalid --frames '{}'. {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--screenshot" && hasValue) {
            options.screenshot = argv[++i];
        } else if (arg == "--gui-click" && hasValue) {
            options.guiClick = argv[++i];
        } else if (arg == "--sim-key" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t comma = text.find(',');
            int frame = 0;
            const SDL_Scancode key = comma == std::string_view::npos
                                         ? SDL_SCANCODE_UNKNOWN
                                         : SDL_GetScancodeFromName(std::string(text.substr(comma + 1)).c_str());
            if (key == SDL_SCANCODE_UNKNOWN || !parseInt(text.substr(0, comma), frame)) {
                kast::log::error(std::format("Invalid --sim-key '{}', expected frame,key (an SDL key name such as F3). {}",
                                             text, kUsage));
                return std::nullopt;
            }
            options.simKeys.emplace_back(frame, key);
        } else if (arg == "--sim-click" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t first = text.find(','), second = first == std::string_view::npos ? first : text.find(',', first + 1);
            int frame = 0, x = -1, y = -1;
            const auto coordinate = [](std::string_view part, int& out) {
                const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), out);
                return ec == std::errc{} && ptr == part.data() + part.size() && out >= 0;
            };
            if (second == std::string_view::npos || !parseInt(text.substr(0, first), frame) ||
                !coordinate(text.substr(first + 1, second - first - 1), x) || !coordinate(text.substr(second + 1), y)) {
                kast::log::error(std::format("Invalid --sim-click '{}', expected frame,x,y (window units). {}", text, kUsage));
                return std::nullopt;
            }
            options.simClicks.emplace_back(frame, SDL_FPoint{static_cast<float>(x), static_cast<float>(y)});
        } else if (arg == "--size" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t x = text.find('x');
            int width = 0, height = 0;
            if (x == std::string_view::npos || !parseInt(text.substr(0, x), width) || !parseInt(text.substr(x + 1), height)) {
                kast::log::error(std::format("Invalid --size '{}', expected WxH. {}", text, kUsage));
                return std::nullopt;
            }
            options.size = std::pair{width, height};
        } else if (arg == "--window-pos" && hasValue) {
            const std::string_view text = argv[++i];
            options.windowPos = kast::parseWindowPos(text);
            if (!options.windowPos) {
                kast::log::error(std::format("Invalid --window-pos '{}', expected X,Y. {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--user-settings" && hasValue) {
            options.userSettings = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[++i]));
        } else if (arg == "--fixed-dt" && hasValue) { // deterministic screenshots of anything that moves
            const std::string_view text = argv[++i];
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), options.fixedDt);
            if (ec != std::errc{} || ptr != text.data() + text.size() || !(options.fixedDt > 0.0f)) {
                kast::log::error(std::format("Invalid --fixed-dt '{}', expected a positive number of seconds. {}", text,
                                             kUsage));
                return std::nullopt;
            }
        } else if (arg == "--gpu-stats") {
            options.gpuStats = true;
        } else if (arg == "--log-sdfgi") {
            kast::setSdfgiLogging(true); // each frame's SDFGI rebuilt cells and probes (sdfgi.h)
        } else if (arg == "--no-instancing") {
            options.noInstancing = true;
        } else if (arg == "--no-culling") {
            options.noCulling = true;
        } else if (arg == "--no-lod") {
            options.noLod = true;
        } else if (arg == "--no-occlusion") {
            options.noOcclusion = true;
        } else if (arg == "--lod-bias" && hasValue) {
            const std::string_view text = argv[++i];
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), options.lodBias);
            if (ec != std::errc{} || ptr != text.data() + text.size() || !(options.lodBias > 0.0f)) {
                kast::log::error(std::format("Invalid --lod-bias '{}', expected a positive number. {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--force-lod" && hasValue) {
            const std::string_view text = argv[++i];
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), options.forceLod);
            if (ec != std::errc{} || ptr != text.data() + text.size() || options.forceLod < 0) {
                kast::log::error(std::format("Invalid --force-lod '{}', expected a level (0, 1, ...). {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--no-audio-device") {
            options.noAudioDevice = true;
        } else {
            kast::log::error(std::format("Invalid argument '{}'. {}", arg, kUsage));
            return std::nullopt;
        }
    }
    if (!options.screenshot.empty() && options.frames == 0) {
        kast::log::error("--screenshot requires --frames N");
        return std::nullopt;
    }
    return options;
}

// Loads the project, its Online Services auth config (nullopt while disabled) and its startScene from the pak next to
// the exe. False + error (a readable message) on failure.
bool loadGame(kast::Project& project, std::optional<kast::AuthConfig>& auth, kast::Scene& scene, std::string& error)
{
    const char* base = SDL_GetBasePath();
    if (!base) {
        error = std::format("Cannot find the game's folder: {}", SDL_GetError());
        return false;
    }
    const std::filesystem::path root = std::filesystem::path(base).parent_path(); // base ends with a separator
    kast::Pak pak;
    if (!pak.open(root / kPakFileName, error))
        return false;
    kast::mountPak(std::move(pak), root);
    if (!kast::loadProject(root / kast::kProjectFileName, project, error))
        return false;
    auth = kast::authConfig(project, error);
    if (!error.empty())
        return false;
    if (auth)
        kast::log::info(std::format("Online Services enabled: auth {}{}", auth->url,
                                    auth->caPem.empty() ? "" : std::format(", CA file '{}'", *project.online.caFile)));
    if (!project.startScene) {
        error = std::format("Project '{}' has no startScene", project.name);
        return false;
    }
    std::string reason;
    const std::optional<std::filesystem::path> file = kast::resolveProjectPath(project.root, *project.startScene, reason);
    if (!file) {
        error = std::format("Invalid startScene: {}", reason);
        return false;
    }
    kast::activateComponentDefinitions(project.root, nullptr); // before the scene loads, so its Lua components resolve
    return kast::loadScene(file->string(), scene, error, nullptr, project.root);
}

// The player's own display settings file: <SDL_GetPrefPath("Kast", project name)>settings.json, i.e.
// %APPDATA%\Kast\<project name>\settings.json (docs/engine/game-gui.html#user-settings). Empty if SDL can't tell.
std::filesystem::path userSettingsFile(const kast::Project& project)
{
    char* folder = SDL_GetPrefPath("Kast", kast::prefFolderName(project.name).c_str());
    if (!folder) {
        kast::log::warn(std::format("No folder for user settings ({}): they are not kept", SDL_GetError()));
        return {};
    }
    const std::filesystem::path file = std::filesystem::path(reinterpret_cast<const char8_t*>(folder)) / "settings.json";
    SDL_free(folder);
    return file;
}

} // namespace

int main(int argc, char** argv)
{
    kast::disableCrashDialogsIfRequested();
    const std::optional<Options> options = parseArgs(argc, argv);
    if (!options)
        return 2;

    kast::Project project;
    std::optional<kast::AuthConfig> auth;
    kast::Scene scene;
    if (std::string error; !loadGame(project, auth, scene, error)) {
        kast::log::error(error);
        if (options->frames == 0) // a --frames run (tests) has no one to close the box
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game failed to start", error.c_str(), nullptr);
        return 1;
    }

    // The window: the project's, under the player's own choices, under --size.
    const std::filesystem::path settingsFile = options->userSettings.empty() ? userSettingsFile(project) : options->userSettings;
    kast::UserDisplaySettings userSettings = settingsFile.empty() ? kast::UserDisplaySettings{}
                                                                  : kast::loadUserDisplaySettings(settingsFile);
    if (!settingsFile.empty())
        kast::log::info(std::format("User settings: {}", settingsFile.string()));
    kast::DisplayMode startMode = kast::applyUserDisplay(project.gui.window, userSettings);
    if (options->size)
        startMode = {.mode = kast::WindowMode::Windowed, .width = options->size->first, .height = options->size->second};
    kast::GuiScale guiScale = project.gui.scale;
    guiScale.uiScale = std::clamp(userSettings.uiScale.value_or(guiScale.uiScale), kast::kMinUiScale, kast::kMaxUiScale);

    if (options->noAudioDevice)
        kast::Audio::setDefaultOutput(kast::Audio::Output::Null);
    kast::LuaSystems lua;
    kast::Input input; // window focus loss clears it (Input::onEvent)
    kast::CursorLock cursorLock;
    kast::Renderer renderer;
    kast::PostProcess post(kGameView);
    double gpuMs = 0.0; // --gpu-stats: summed after the warmup
    double voxelGiMs = 0.0; // --gpu-stats: the VoxelGI view's share (its volumes' compute passes)
    double sdfgiMs = 0.0;   // --gpu-stats: the SDFGI view's (its cascades' compute passes)
    std::vector<double> frameMs, sdfgiFrameMs; // --gpu-stats: each frame's, and its SDFGI view's (for percentiles)
    int gpuFrames = 0;
    kast::SceneGui gui("game");
    kast::WindowDisplay display;
    kast::StatsOverlay stats(options->gpuStats ? BGFX_DEBUG_PROFILER : BGFX_DEBUG_NONE); // F3
    // Scene and GUI draw on the backbuffer until a GUI reads what is under it (backdrop-filter), then into this
    // sampleable target, copied onto the screen after the GUI. ponytail: switched the frame after the first read, so
    // that one frame draws the backdrop without its effect.
    bgfx::FrameBufferHandle readable = BGFX_INVALID_HANDLE;
    int readableWidth = 0;
    int readableHeight = 0;
    SDL_Window* window = nullptr;
    uint64_t lastTicks = 0;
    kast::ShaderClock shaderClock; // u_time
    int frame = 0;
    bool screenshotRequested = false;
    bool screenshotSaved = false;
    bool failed = false;

    kast::AppHooks hooks;
    hooks.init = [&](SDL_Window* created) {
        window = created;
        kast::log::info(std::format("Window title: {}", SDL_GetWindowTitle(window)));
        renderer.init();
        renderer.setInstancing(!options->noInstancing);
        renderer.setCulling(!options->noCulling);
        renderer.setLod(!options->noLod);
        renderer.setLodBias(options->lodBias);
        renderer.setForcedLod(options->forceLod);
        renderer.setOcclusion(!options->noOcclusion);
        post.init();
        kast::gui::init();
        display.attach(window);
        if (startMode.mode != kast::WindowMode::Windowed)
            display.setMode(startMode);
        gui.setScale(guiScale);
        gui.setDisplayScale(display.displayScale());
        // What Lua changes from now on is the player's: kept in the settings file.
        const auto save = [&] {
            if (!settingsFile.empty())
                kast::saveUserDisplaySettings(settingsFile, userSettings);
        };
        display.setListener([&, save](const kast::DisplayMode& mode) {
            userSettings.mode = mode.mode;
            userSettings.width = mode.width;
            userSettings.height = mode.height;
            userSettings.refresh = mode.refresh;
            save();
        });
        gui.setScaleListener([&, save](const kast::GuiScale& scale) {
            if (userSettings.uiScale.value_or(guiScale.uiScale) == scale.uiScale)
                return;
            userSettings.uiScale = scale.uiScale;
            save();
        });
        lua.setGui(&gui); // kast.gui.*
        lua.setDisplay(&display); // kast.display.*
        if (options->gpuStats)
            bgfx::setDebug(BGFX_DEBUG_PROFILER);
        if (kast::activeCamera(scene) == entt::null)
            kast::log::warn("No Camera in scene: nothing to render");
        lua.start(scene, project.root, *project.startScene, &input,
                  project.stateMachines.value_or(std::vector<std::string>{}), auth);
        lua.audio().setMix(project.audio);
        lastTicks = SDL_GetTicksNS();
        return true;
    };
    hooks.event = [&](const SDL_Event& event) {
        int windowWidth = 0;
        int windowHeight = 0;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight); // event coordinates are window, not pixel, units
        input.view = {0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight)};
        input.onEvent(event);
        gui.onEvent(event, 0.0f, 0.0f, SDL_GetWindowPixelDensity(window)); // the GUI is in pixels
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED || event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
            gui.setDisplayScale(display.displayScale()); // another monitor or a changed Windows scale
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F3 && !event.key.repeat)
            stats.toggle();
    };
    hooks.frame = [&] {
        const uint64_t ticks = SDL_GetTicksNS();
        const double dt = options->fixedDt > 0.0f ? options->fixedDt : static_cast<double>(ticks - lastTicks) * 1e-9;
        stats.frame(dt, lua.world().rttMs(), &renderer.stats()); // the last frame's
        shaderClock.advance(dt);
        renderer.setTime(shaderClock.seconds());
        if (kast::tickPlay(scene, lua, static_cast<float>(dt)))
            gui.clear(); // kast.loadScene: the new scene's GUIs load below
        gui.sync(scene, project.root, lua.bus());
        kast::gui::syncTextInput(window, gui.focusedField().has_value());
        cursorLock.sync(window, input.cursorLocked);
        lastTicks = ticks;
        input.endFrame();

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        post.begin(static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                   kast::sceneClearColor(scene.postProcessing, kClearColor), scene.postProcessing, true);
        if (const entt::entity camera = kast::activeCamera(scene); camera != entt::null && width > 0 && height > 0) {
            float view[16], proj[16];
            kast::cameraMatrices(scene, camera, static_cast<float>(width) / static_cast<float>(height),
                                 bgfx::getCaps()->homogeneousDepth, view, proj);
            post.setProjection(proj);
            renderer.setTarget(post.target(), post.width(), post.height(), post.blendView(),
                               post.volumetricFogView());
            gui.world().layout(scene, view, proj, width, height, &lua.physics());
            renderer.setWorldQuads(gui.world().atlasTexture(), gui.world().quads());
            renderer.render(scene, project.root, post.sceneView(), view, proj, kShadowViews);
        }
        if (kast::gui::wantsReadableTarget() && width > 0 && height > 0 &&
            (!bgfx::isValid(readable) || width != readableWidth || height != readableHeight)) {
            if (bgfx::isValid(readable))
                bgfx::destroy(readable);
            readable = bgfx::createFrameBuffer(static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                                               bgfx::TextureFormat::BGRA8,
                                               BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            readableWidth = width;
            readableHeight = height;
            kast::log::info(std::format("Drawing through a readable {}x{} framebuffer (a GUI reads its backdrop)", width, height));
        }
        post.apply(scene.postProcessing, true, readable);
        // The last frame's views (the profiler's per-view timers), so the vsync wait isn't counted; a frame with a timer
        // not read back yet is left out.
        if (const std::optional<double> ms = kast::viewGpuMs(nullptr); options->gpuStats && frame >= kGpuStatsWarmupFrames && ms) {
            gpuMs += *ms;
            voxelGiMs += kast::viewGpuMs("VoxelGI").value_or(0.0);
            const double sdfgi = kast::viewGpuMs("SDFGI").value_or(0.0);
            sdfgiMs += sdfgi;
            frameMs.push_back(*ms);
            sdfgiFrameMs.push_back(sdfgi);
            ++gpuFrames;
        }
        if (width > 0 && height > 0) {
            gui.resize(width, height);
            gui.render(kGuiView, readable);
            if (bgfx::isValid(readable))
                kast::gui::present(kPresentView, bgfx::getTexture(readable), width, height);
        }

        // ponytail: same --frames/--screenshot sequence as editor/main.cpp; move into kast::run if a third host needs it.
        ++frame;
        for (const auto& [keyFrame, scancode] : options->simKeys) { // handled from the next frame, like a real press
            if (frame != keyFrame)
                continue;
            SDL_Event event{};
            event.key = {.type = SDL_EVENT_KEY_DOWN, .windowID = SDL_GetWindowID(window), .scancode = scancode, .down = true};
            SDL_PushEvent(&event);
            event.key.type = SDL_EVENT_KEY_UP;
            event.key.down = false;
            SDL_PushEvent(&event);
        }
        for (const auto& [clickFrame, at] : options->simClicks) { // a move there, a press and a release, as a real click
            if (frame != clickFrame)
                continue;
            SDL_Event event{};
            event.motion = {.type = SDL_EVENT_MOUSE_MOTION, .windowID = SDL_GetWindowID(window), .x = at.x, .y = at.y};
            SDL_PushEvent(&event);
            event.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN, .windowID = SDL_GetWindowID(window),
                            .button = SDL_BUTTON_LEFT, .down = true, .clicks = 1, .x = at.x, .y = at.y};
            SDL_PushEvent(&event);
            event.button.type = SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.down = false;
            SDL_PushEvent(&event);
        }
        if (!options->guiClick.empty() && frame == kGuiClickFrame && !gui.click(options->guiClick))
            kast::log::error(std::format("--gui-click {}: no such element in a visible GUI", options->guiClick));
        if (options->frames == 0 || frame < options->frames)
            return;
        if (options->screenshot.empty()) {
            if (frame == options->frames)
                kast::requestQuit();
        } else if (!screenshotRequested) {
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, options->screenshot.c_str());
            screenshotRequested = true;
        } else if (frame == options->frames + kScreenshotTimeoutFrames) {
            kast::log::error("Screenshot timed out");
            failed = true;
            kast::requestQuit();
        }
    };
    hooks.screenshotSaved = [&](const char*, bool saved) {
        screenshotSaved = saved;
        kast::requestQuit();
    };
    hooks.shutdown = [&] {
        if (options->gpuStats && gpuFrames > 0)
            kast::log::info(std::format("GPU frame time: {:.3f} ms average over {} frames (VoxelGI view {:.3f} ms)",
                                        gpuMs / gpuFrames, gpuFrames, voxelGiMs / gpuFrames));
        // The 99th percentile and the slowest: a lone slow frame (the OS or another program on the GPU) shows in the
        // latter only.
        const auto percentile = [](std::vector<double> ms, double p) {
            std::ranges::sort(ms);
            return ms[std::min(ms.size() - 1, static_cast<size_t>(p * static_cast<double>(ms.size())))];
        };
        if (options->gpuStats && gpuFrames > 0 && scene.lighting.sdfgi.enabled)
            kast::log::info(std::format("SDFGI view GPU time: {:.3f} ms average, {:.3f} ms 99th percentile, {:.3f} ms at "
                                        "most over {} frames (frame: {:.3f} ms 99th percentile, {:.3f} ms at most)",
                                        sdfgiMs / gpuFrames, percentile(sdfgiFrameMs, 0.99), percentile(sdfgiFrameMs, 1.0),
                                        gpuFrames, percentile(frameMs, 0.99), percentile(frameMs, 1.0)));
        kast::log::info(kast::assetLoadSummary());
        lua.stop();
        gui.release();
        kast::gui::shutdown();
        if (bgfx::isValid(readable))
            bgfx::destroy(readable);
        post.shutdown();
        renderer.shutdown();
    };

    const int code = kast::run({.title = project.name.c_str(), .width = startMode.width, .height = startMode.height,
                                .activate = options->frames == 0, .position = options->windowPos},
                               hooks);
    if (code != 0)
        return code;
    if (!options->screenshot.empty() && !screenshotSaved)
        failed = true;
    return failed ? 1 : 0;
}
