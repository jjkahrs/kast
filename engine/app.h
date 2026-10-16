#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <utility>

union SDL_Event;
struct SDL_Window;

namespace kast {

struct AppConfig {
    const char* title = "Kast";
    int width = 1280;
    int height = 720;
    bool vsync = true; // false: present as fast as possible (frame-rate measurements)
    bool activate = true; // false (--frames test runs): show without taking focus; init failures only log
    // Top-left corner of the window in desktop coordinates (--window-pos). Empty: the environment variable
    // KAST_WINDOW_POS ("X,Y", how ctest keeps test windows off the main monitor), else where the OS puts it.
    std::optional<std::pair<int, int>> position;
};

// "X,Y": two integers (negative = left of / above the main monitor). Empty if text is anything else.
std::optional<std::pair<int, int>> parseWindowPos(std::string_view text);

// Lifecycle hooks the host (editor, game client) plugs into. All optional.
struct AppHooks {
    std::function<bool(SDL_Window*)> init;            // after bgfx init; return false to abort
    std::function<void(const SDL_Event&)> event;      // every SDL event
    std::function<void()> frame;                      // once per frame, before bgfx::frame()
    std::function<void()> shutdown;                   // before bgfx shutdown
    std::function<void(const char* path, bool saved)> screenshotSaved; // after bgfx::requestScreenShot completes
    std::function<bool()> quitRequested;              // on SDL_EVENT_QUIT; return false to keep running
};

// Runs until SDL_EVENT_QUIT (unless quitRequested vetoes it). Returns 0 on clean exit, nonzero on init failure.
int run(const AppConfig& config, const AppHooks& hooks);

// Posts SDL_EVENT_QUIT; same path as closing the window.
void requestQuit();

} // namespace kast
