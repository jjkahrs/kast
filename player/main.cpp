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

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>

#include <charconv>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr bgfx::ViewId kShadowViews = 0; // the scene's shadow maps (Renderer::kShadowViews)
constexpr bgfx::ViewId kGameView = kShadowViews + kast::Renderer::kShadowViews; // then its post passes
constexpr bgfx::ViewId kGuiView = kGameView + kast::PostProcess::kViews; // the scene's game GUIs, over the scene
constexpr uint32_t kClearColor = 0x202020ff;
constexpr int kScreenshotTimeoutFrames = 30;
constexpr const char* kPakFileName = "game.pak";
constexpr int kGuiClickFrame = 10; // --gui-click: once the GUIs have loaded and laid out
constexpr const char* kUsage =
    "Usage: <game>.exe [--frames N] [--screenshot out.png] [--gui-click id] [--size WxH] [--gpu-stats]";
constexpr int kGpuStatsWarmupFrames = 10; // --gpu-stats: skip startup frames (shader and target creation)

struct Options {
    int frames = 0;         // 0 = run until quit
    std::string screenshot; // captured after the last frame
    std::string guiClick;   // tests: click this GUI element id on frame kGuiClickFrame
    int width = kast::AppConfig{}.width, height = kast::AppConfig{}.height; // --size: window size
    bool gpuStats = false;  // log the average GPU frame time on exit
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
        } else if (arg == "--size" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t x = text.find('x');
            if (x == std::string_view::npos || !parseInt(text.substr(0, x), options.width) ||
                !parseInt(text.substr(x + 1), options.height)) {
                kast::log::error(std::format("Invalid --size '{}', expected WxH. {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--gpu-stats") {
            options.gpuStats = true;
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

// Loads the project and its startScene from the pak next to the exe. False + error (a readable message) on failure.
bool loadGame(kast::Project& project, kast::Scene& scene, std::string& error)
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
    return kast::loadScene(file->string(), scene, error);
}

} // namespace

int main(int argc, char** argv)
{
    kast::disableCrashDialogsIfRequested();
    const std::optional<Options> options = parseArgs(argc, argv);
    if (!options)
        return 2;

    kast::Project project;
    kast::Scene scene;
    if (std::string error; !loadGame(project, scene, error)) {
        kast::log::error(error);
        if (options->frames == 0) // a --frames run (tests) has no one to close the box
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game failed to start", error.c_str(), nullptr);
        return 1;
    }

    kast::LuaSystems lua;
    kast::Input input;
    kast::Renderer renderer;
    kast::PostProcess post(kGameView);
    double gpuMs = 0.0; // --gpu-stats: summed after the warmup
    int gpuFrames = 0;
    kast::SceneGui gui("game");
    SDL_Window* window = nullptr;
    uint64_t lastTicks = 0;
    int frame = 0;
    bool screenshotRequested = false;
    bool screenshotSaved = false;
    bool failed = false;

    kast::AppHooks hooks;
    hooks.init = [&](SDL_Window* created) {
        window = created;
        kast::log::info(std::format("Window title: {}", SDL_GetWindowTitle(window)));
        renderer.init();
        post.init();
        kast::gui::init();
        if (options->gpuStats)
            bgfx::setDebug(BGFX_DEBUG_PROFILER);
        if (kast::activeCamera(scene) == entt::null)
            kast::log::warn("No Camera in scene: nothing to render");
        lua.start(scene, project.root, *project.startScene, &input,
                  project.stateMachines.value_or(std::vector<std::string>{}));
        lastTicks = SDL_GetTicksNS();
        return true;
    };
    hooks.event = [&](const SDL_Event& event) {
        input.onEvent(event);
        gui.context().onEvent(event);
    };
    hooks.frame = [&] {
        const uint64_t ticks = SDL_GetTicksNS();
        if (kast::tickPlay(scene, lua, static_cast<float>(ticks - lastTicks) * 1e-9f))
            gui.clear(); // kast.loadScene: the new scene's GUIs load below
        gui.sync(scene, project.root, lua.bus());
        lastTicks = ticks;
        input.endFrame();

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        post.begin(static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                   kast::sceneClearColor(scene.postProcessing, kClearColor));
        if (const entt::entity camera = kast::activeCamera(scene); camera != entt::null && width > 0 && height > 0) {
            float view[16], proj[16];
            kast::cameraMatrices(scene, camera, static_cast<float>(width) / static_cast<float>(height),
                                 bgfx::getCaps()->homogeneousDepth, view, proj);
            renderer.render(scene, project.root, post.sceneView(), view, proj, kShadowViews);
        }
        post.apply(scene.postProcessing, true, BGFX_INVALID_HANDLE);
        if (const bgfx::Stats* stats = bgfx::getStats(); options->gpuStats && frame >= kGpuStatsWarmupFrames) {
            // The last frame's views (the profiler's per-view timers), so the vsync wait isn't counted.
            for (uint16_t i = 0; i < stats->numViews; ++i)
                gpuMs += static_cast<double>(stats->viewStats[i].gpuTimeEnd - stats->viewStats[i].gpuTimeBegin) *
                         1000.0 / static_cast<double>(stats->gpuTimerFreq);
            ++gpuFrames;
        }
        if (width > 0 && height > 0) {
            gui.context().resize(width, height);
            gui.context().render(kGuiView, BGFX_INVALID_HANDLE);
        }

        // ponytail: same --frames/--screenshot sequence as editor/main.cpp; move into kast::run if a third host needs it.
        ++frame;
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
            kast::log::info(std::format("GPU frame time: {:.3f} ms average over {} frames", gpuMs / gpuFrames, gpuFrames));
        lua.stop();
        gui.clear();
        gui.context().release();
        kast::gui::shutdown();
        post.shutdown();
        renderer.shutdown();
    };

    const int code = kast::run({.title = project.name.c_str(), .width = options->width, .height = options->height,
                                .activate = options->frames == 0},
                               hooks);
    if (code != 0)
        return code;
    if (!options->screenshot.empty() && !screenshotSaved)
        failed = true;
    return failed ? 1 : 0;
}
