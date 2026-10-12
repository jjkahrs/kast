#include "app.h"
#include "log.h"

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bimg/bimg.h>
#include <bx/file.h>

#include <cstdarg>
#include <cstdio>
#include <format>
#include <string>

namespace kast {
namespace {

constexpr uint32_t kResetFlags = BGFX_RESET_VSYNC;

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

int fail(const std::string& message, SDL_Window* window)
{
    log::error(message);
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

int run(const AppConfig& config, const AppHooks& hooks)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return fail(std::format("SDL_Init failed: {}", SDL_GetError()), nullptr);
    log::info("SDL initialized");

    SDL_Window* window = SDL_CreateWindow(config.title, config.width, config.height, SDL_WINDOW_RESIZABLE);
    if (!window) {
        int code = fail(std::format("SDL_CreateWindow failed: {}", SDL_GetError()), nullptr);
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
    init.resolution.reset = kResetFlags;
    init.callback = &callback;
    if (!bgfx::init(init)) {
        int code = fail("bgfx::init failed (Direct3D11)", window);
        shutdownSdl(window);
        return code;
    }
    log::info(std::format("bgfx initialized: {} {}x{}", bgfx::getRendererName(bgfx::getRendererType()), width, height));

    if (hooks.init && !hooks.init(window)) {
        int code = fail("Host init hook failed (see log)", window);
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
                bgfx::reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height), kResetFlags);
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
