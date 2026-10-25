#include "window_display.h"

#include "log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace kast {
namespace {

int wholeHz(float refresh) { return static_cast<int>(std::lround(refresh)); }

std::string describe(const DisplayMode& mode)
{
    std::string text = std::format("{} {}x{}", windowModeName(mode.mode), mode.width, mode.height);
    if (mode.refresh)
        text += std::format("@{}", *mode.refresh);
    return text;
}

} // namespace

bool WindowDisplay::setMode(const DisplayMode& mode)
{
    if (!window_)
        return false;
    const auto failed = [&](std::string_view why) {
        log::warn(std::format("kast.display.setMode {}: {}; the mode is unchanged", describe(mode), why));
        return false;
    };
    switch (mode.mode) {
    case WindowMode::Windowed:
        if (!SDL_SetWindowFullscreen(window_, false) || !SDL_SetWindowSize(window_, mode.width, mode.height))
            return failed(SDL_GetError());
        break;
    case WindowMode::Borderless:
        if (!SDL_SetWindowFullscreenMode(window_, nullptr) || !SDL_SetWindowFullscreen(window_, true))
            return failed(SDL_GetError());
        break;
    case WindowMode::Exclusive: {
        int count = 0;
        SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window_), &count);
        const std::optional<DisplayModeInfo> pick = pickExclusiveMode(mode, modes());
        const SDL_DisplayMode* chosen = nullptr;
        for (int i = 0; pick && i < count && !chosen; ++i)
            if (list[i]->w == pick->width && list[i]->h == pick->height && wholeHz(list[i]->refresh_rate) == pick->refresh)
                chosen = list[i];
        const bool set = chosen && SDL_SetWindowFullscreenMode(window_, chosen) && SDL_SetWindowFullscreen(window_, true);
        SDL_free(list);
        if (!chosen)
            return failed("the window's display has no such mode (see kast.display.modes())");
        if (!set)
            return failed(SDL_GetError());
        break;
    }
    }
    SDL_SyncWindow(window_);
    current_ = mode;
    log::info(std::format("Display mode: {}", describe(this->mode())));
    if (listener_)
        listener_(mode);
    return true;
}

DisplayMode WindowDisplay::mode()
{
    DisplayMode out = current_;
    if (!window_)
        return out;
    const SDL_DisplayMode* fullscreen = SDL_GetWindowFullscreenMode(window_);
    if (!(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN))
        out.mode = WindowMode::Windowed;
    else
        out.mode = fullscreen ? WindowMode::Exclusive : WindowMode::Borderless;
    out.refresh.reset();
    if (out.mode == WindowMode::Exclusive) {
        out.width = fullscreen->w;
        out.height = fullscreen->h;
        out.refresh = wholeHz(fullscreen->refresh_rate);
    } else {
        SDL_GetWindowSize(window_, &out.width, &out.height);
    }
    return out;
}

std::vector<DisplayModeInfo> WindowDisplay::modes()
{
    std::vector<DisplayModeInfo> out;
    if (!window_)
        return out;
    int count = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window_), &count);
    for (int i = 0; i < count; ++i) { // SDL lists a size and rate once per pixel format / density: kept once
        const DisplayModeInfo mode{list[i]->w, list[i]->h, wholeHz(list[i]->refresh_rate)};
        if (std::ranges::find(out, mode) == out.end())
            out.push_back(mode);
    }
    SDL_free(list);
    return out;
}

double WindowDisplay::displayScale()
{
    const float scale = window_ ? SDL_GetWindowDisplayScale(window_) : 0.0f;
    return scale > 0.0f ? scale : 1.0;
}

} // namespace kast
