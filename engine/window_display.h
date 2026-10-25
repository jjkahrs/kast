#pragma once

#include "display_access.h"

#include <functional>

struct SDL_Window;

// kast.display on a real SDL window (kast_player): windowed at a size, borderless fullscreen on the desktop, or
// exclusive fullscreen in one of the window's display's modes.
namespace kast {

class WindowDisplay final : public DisplayAccess {
public:
    void attach(SDL_Window* window) { window_ = window; }
    bool setMode(const DisplayMode& mode) override;
    DisplayMode mode() override;
    std::vector<DisplayModeInfo> modes() override;
    double displayScale() override;
    // Called after each successful setMode with the mode asked for (kast_player saves it).
    void setListener(std::function<void(const DisplayMode&)> listener) { listener_ = std::move(listener); }

private:
    SDL_Window* window_ = nullptr;
    DisplayMode current_; // as last set (its size: the windowed size to return to)
    std::function<void(const DisplayMode&)> listener_;
};

} // namespace kast
