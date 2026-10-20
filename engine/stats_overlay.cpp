#include "stats_overlay.h"

#include <bgfx/bgfx.h>

#include <cmath>
#include <format>

namespace kast {

void FpsAverage::addFrame(double dt)
{
    frames_.push_back(dt);
    total_ += dt;
    while (frames_.size() > 1 && total_ - frames_.front() >= kWindow) { // the rest still covers the window
        total_ -= frames_.front();
        frames_.pop_front();
    }
}

int FpsAverage::fps() const
{
    return total_ > 0.0 ? static_cast<int>(std::lround(static_cast<double>(frames_.size()) / total_)) : 0;
}

std::string statsOverlayText(int fps, std::optional<int> pingMs)
{
    return std::format("FPS {}  Ping {} ms", fps, pingMs ? std::to_string(*pingMs) : std::string("--"));
}

void StatsOverlay::toggle()
{
    visible_ = !visible_;
    bgfx::setDebug(debugFlags_ | (visible_ ? BGFX_DEBUG_TEXT : 0));
}

void StatsOverlay::frame(double dt, std::optional<int> pingMs)
{
    fps_.addFrame(dt);
    if (!visible_)
        return;
    bgfx::dbgTextClear();
    const std::string text = statsOverlayText(fps_.fps(), pingMs);
    // White on dark gray: palette 0 (black) is transparent.
    bgfx::dbgTextPrintf(0, bgfx::getStats()->textHeight - 1, 0x8f, "%s", text.c_str());
}

} // namespace kast
