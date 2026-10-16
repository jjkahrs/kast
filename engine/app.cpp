#include "app.h"
#include "log.h"

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bimg/bimg.h>
#include <bx/file.h>

#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <string>

namespace kast {
namespace {


// Creates the window at config.position, else at KAST_WINDOW_POS, else where the OS puts it.
SDL_Window* createWindow(const AppConfig& config)
{
    std::optional<std::pair<int, int>> position = config.position;
    if (const char* env = SDL_getenv("KAST_WINDOW_POS"); !position && env && *env) {
        position = parseWindowPos(env);
        if (!position)
            log::warn(std::format("Ignored KAST_WINDOW_POS '{}': expected X,Y", env));
    }
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, config.title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, config.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, config.height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    if (position) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, position->first);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, position->second);
    }
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window && position) {
        int x = 0, y = 0;
        SDL_GetWindowPosition(window, &x, &y);
        const char* display = SDL_GetDisplayName(SDL_GetDisplayForWindow(window));
        log::info(std::format("Window at {}, {} on display '{}'", x, y, display ? display : "?"));
    }
    return window;
}

// Routes bgfx callbacks to the logger and writes screenshots as PNG.
class BgfxCallback final : public bgfx::CallbackI {
public:
    explicit BgfxCallback(const AppHooks& hooks) : hooks_(hooks) {}

    void fatal(const char* filePath, uint16_t line, bgfx::Fatal::Enum, const char* str) override
    {
        log::error(std::format("bgfx fatal {}({}): {}", filePath, line, str));
    }
    void traceVargs(const char*, uint16_t, const char*, va_list) override {}
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}

    void screenShot(const char* filePath, uint32_t width, uint32_t height, uint32_t pitch,
                    const void* data, uint32_t /*size*/, bool yflip) override
    {
        bool saved = false;
        bx::FileWriter writer;
        if (bx::open(&writer, filePath)) {
            bx::Error err;
            bimg::imageWritePng(&writer, width, height, pitch, data, bimg::TextureFormat::BGRA8, yflip, &err);
            bx::close(&writer);
            saved = err.isOk();
        }
        if (saved)
            log::info(std::format("Screenshot saved: {} ({}x{})", filePath, width, height));
        else
            log::error(std::format("Screenshot failed: {}", filePath));
        if (hooks_.screenshotSaved)
            hooks_.screenshotSaved(filePath, saved);
    }

private:
    const AppHooks& hooks_;
};

int fail(const AppConfig& config, const std::string& message, SDL_Window* window)
{
    log::error(message);
    if (config.activate) // a --frames run (tests) has no one to close the box
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Kast failed to start", message.c_str(), window);
    return 1;
}

void shutdownSdl(SDL_Window* window)
{
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
    log::info("Shutdown: SDL");
}

} // namespace

std::optional<std::pair<int, int>> parseWindowPos(std::string_view text)
{
    const size_t comma = text.find(',');
    if (comma == std::string_view::npos)
        return std::nullopt;
    const auto number = [](std::string_view part, int& out) {
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), out);
        return ec == std::errc{} && end == part.data() + part.size() && !part.empty();
    };
    int x = 0, y = 0;
    if (!number(text.substr(0, comma), x) || !number(text.substr(comma + 1), y))
        return std::nullopt;
    return std::pair{x, y};
}

int run(const AppConfig& config, const AppHooks& hooks)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return fail(config, std::format("SDL_Init failed: {}", SDL_GetError()), nullptr);
    log::info("SDL initialized");

    if (!config.activate) // show without taking focus from the user's app
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_Window* window = createWindow(config);
    if (!window) {
        int code = fail(config, std::format("SDL_CreateWindow failed: {}", SDL_GetError()), nullptr);
        shutdownSdl(nullptr);
        return code;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);

    // Calling renderFrame before init keeps bgfx single-threaded, so callbacks run on this thread.
    bgfx::renderFrame();

    BgfxCallback callback(hooks);
    bgfx::Init init;
    init.type = bgfx::RendererType::Direct3D11;
    init.platformData.nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    init.resolution.width = static_cast<uint32_t>(width);
    init.resolution.height = static_cast<uint32_t>(height);
    const uint32_t resetFlags = config.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
    init.resolution.reset = resetFlags;
    init.callback = &callback;
    if (!bgfx::init(init)) {
        int code = fail(config, "bgfx::init failed (Direct3D11)", window);
        shutdownSdl(window);
        return code;
    }
    log::info(std::format("bgfx initialized: {} {}x{}", bgfx::getRendererName(bgfx::getRendererType()), width, height));

    if (hooks.init && !hooks.init(window)) {
        int code = fail(config, "Host init hook failed (see log)", window);
        bgfx::shutdown();
        shutdownSdl(window);
        return code;
    }

    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT && (!hooks.quitRequested || hooks.quitRequested()))
                running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                width = event.window.data1;
                height = event.window.data2;
                bgfx::reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height), resetFlags);
                log::info(std::format("Backbuffer resized to {}x{}", width, height));
            }
            if (hooks.event)
                hooks.event(event);
        }
        if (!running)
            break;
        if (hooks.frame)
            hooks.frame();
        bgfx::frame();
    }

    log::info("Quit requested, shutting down");
    if (hooks.shutdown)
        hooks.shutdown();
    bgfx::shutdown();
    log::info("Shutdown: bgfx");
    shutdownSdl(window);
    return 0;
}

void requestQuit()
{
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

} // namespace kast
