#include "stats_overlay.h"

#include <bgfx/bgfx.h>

#include <cmath>
#include <cstring>
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

std::string statsOverlayText(int fps, std::optional<int> pingMs, std::optional<double> voxelGiMs)
{
    std::string text = std::format("FPS {}  Ping {} ms", fps, pingMs ? std::to_string(*pingMs) : std::string("--"));
    if (voxelGiMs)
        text += std::format("  VoxelGI {:.2f} ms", *voxelGiMs);
    return text;
}

std::optional<double> viewGpuMs(const char* name, uint16_t first, uint16_t count)
{
    const bgfx::Stats* stats = bgfx::getStats();
    if (stats->gpuTimerFreq <= 0)
        return std::nullopt;
    std::optional<double> ms;
    for (uint16_t i = 0; i < stats->numViews; ++i) {
        const bgfx::ViewStats& view = stats->viewStats[i];
        if ((name && std::strcmp(view.name, name) != 0) || view.view < first || view.view >= first + count)
            continue;
        const double viewMs = double(view.gpuTimeEnd - view.gpuTimeBegin) * 1000.0 / double(stats->gpuTimerFreq);
        if (!(viewMs >= 0.0 && viewMs < 1000.0))
            return std::nullopt; // a timer not read back yet (seen now and then): no time this frame
        ms = ms.value_or(0.0) + viewMs;
    }
    return ms;
}

void StatsOverlay::toggle()
{
    visible_ = !visible_;
    bgfx::setDebug(debugFlags_ | (visible_ ? BGFX_DEBUG_TEXT | BGFX_DEBUG_PROFILER : 0));
}

void StatsOverlay::frame(double dt, std::optional<int> pingMs)
{
    fps_.addFrame(dt);
    if (!visible_)
        return;
    bgfx::dbgTextClear();
    const std::string text = statsOverlayText(fps_.fps(), pingMs, viewGpuMs("VoxelGI"));
    // White on dark gray: palette 0 (black) is transparent.
    bgfx::dbgTextPrintf(0, bgfx::getStats()->textHeight - 1, 0x8f, "%s", text.c_str());
}

} // namespace kast
