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
constexpr bgfx::ViewId kGuiView = kGameView + kast::PostProcess::kViews; // the scene's game GUIs (kast::gui::kViews), over the scene
constexpr bgfx::ViewId kPresentView = kGuiView + kast::gui::kViews; // the readable target onto the screen, if used
constexpr uint32_t kClearColor = 0x202020ff;
constexpr int kScreenshotTimeoutFrames = 30;
constexpr const char* kPakFileName = "game.pak";
constexpr int kGuiClickFrame = 10; // --gui-click: once the GUIs have loaded and laid out
constexpr const char* kUsage =
    "Usage: <game>.exe [--frames N] [--screenshot out.png] [--gui-click id] [--size WxH] [--window-pos X,Y] [--gpu-stats] [--no-instancing] [--no-culling] [--no-audio-device]";
constexpr int kGpuStatsWarmupFrames = 10; // --gpu-stats: skip startup frames (shader and target creation)

struct Options {
    int frames = 0;         // 0 = run until quit
    std::string screenshot; // captured after the last frame
    std::string guiClick;   // tests: click this GUI element id on frame kGuiClickFrame
    int width = kast::AppConfig{}.width, height = kast::AppConfig{}.height; // --size: window size
    std::optional<std::pair<int, int>> windowPos; // --window-pos: window top-left
    bool gpuStats = false;  // log the average GPU frame time on exit
    bool noInstancing = false; // draw every mesh one by one (Renderer::setInstancing)
    bool noCulling = false;    // draw every mesh in every pass (Renderer::setCulling)
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
        } else if (arg == "--size" && hasValue) {
            const std::string_view text = argv[++i];
            const size_t x = text.find('x');
            if (x == std::string_view::npos || !parseInt(text.substr(0, x), options.width) ||
                !parseInt(text.substr(x + 1), options.height)) {
                kast::log::error(std::format("Invalid --size '{}', expected WxH. {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--window-pos" && hasValue) {
            const std::string_view text = argv[++i];
            options.windowPos = kast::parseWindowPos(text);
            if (!options.windowPos) {
                kast::log::error(std::format("Invalid --window-pos '{}', expected X,Y. {}", text, kUsage));
                return std::nullopt;
            }
        } else if (arg == "--gpu-stats") {
            options.gpuStats = true;
        } else if (arg == "--no-instancing") {
            options.noInstancing = true;
        } else if (arg == "--no-culling") {
            options.noCulling = true;
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

    if (options->noAudioDevice)
        kast::Audio::setDefaultOutput(kast::Audio::Output::Null);
    kast::LuaSystems lua;
    kast::Input input; // window focus loss clears it (Input::onEvent)
    kast::CursorLock cursorLock;
    kast::Renderer renderer;
    kast::PostProcess post(kGameView);
    double gpuMs = 0.0; // --gpu-stats: summed after the warmup
    int gpuFrames = 0;
    kast::SceneGui gui("game");
    // Scene and GUI draw on the backbuffer until a GUI reads what is under it (backdrop-filter), then into this
    // sampleable target, copied onto the screen after the GUI. ponytail: switched the frame after the first read, so
    // that one frame draws the backdrop without its effect.
    bgfx::FrameBufferHandle readable = BGFX_INVALID_HANDLE;
    int readableWidth = 0;
    int readableHeight = 0;
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
        renderer.setInstancing(!options->noInstancing);
        renderer.setCulling(!options->noCulling);
        post.init();
        kast::gui::init();
        lua.setGui(&gui); // kast.gui.*
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
        gui.context().onEvent(event);
    };
    hooks.frame = [&] {
        const uint64_t ticks = SDL_GetTicksNS();
        if (kast::tickPlay(scene, lua, static_cast<float>(ticks - lastTicks) * 1e-9f))
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
                   kast::sceneClearColor(scene.postProcessing, kClearColor));
        if (const entt::entity camera = kast::activeCamera(scene); camera != entt::null && width > 0 && height > 0) {
            float view[16], proj[16];
            kast::cameraMatrices(scene, camera, static_cast<float>(width) / static_cast<float>(height),
                                 bgfx::getCaps()->homogeneousDepth, view, proj);
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
        if (const bgfx::Stats* stats = bgfx::getStats(); options->gpuStats && frame >= kGpuStatsWarmupFrames) {
            // The last frame's views (the profiler's per-view timers), so the vsync wait isn't counted.
            for (uint16_t i = 0; i < stats->numViews; ++i)
                gpuMs += static_cast<double>(stats->viewStats[i].gpuTimeEnd - stats->viewStats[i].gpuTimeBegin) *
                         1000.0 / static_cast<double>(stats->gpuTimerFreq);
            ++gpuFrames;
        }
        if (width > 0 && height > 0) {
            gui.context().resize(width, height);
            gui.context().render(kGuiView, readable);
            if (bgfx::isValid(readable))
                kast::gui::present(kPresentView, bgfx::getTexture(readable), width, height);
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
        if (bgfx::isValid(readable))
            bgfx::destroy(readable);
        post.shutdown();
        renderer.shutdown();
    };

    const int code = kast::run({.title = project.name.c_str(), .width = options->width, .height = options->height,
                                .activate = options->frames == 0, .position = options->windowPos},
                               hooks);
    if (code != 0)
        return code;
    if (!options->screenshot.empty() && !screenshotSaved)
        failed = true;
    return failed ? 1 : 0;
}
