#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

// The play stats overlay (F3): one line, "FPS 144  Ping 42 ms" (and "  VoxelGI 0.31 ms" while VoxelGI volumes relight,
// "  SDFGI 1.20 ms" while SDFGI is on: the GPU time of their views, so the profiler runs while it shows; then the
// renderer's camera-pass counts, renderStatsText), in bgfx debug text at the bottom-left of the
// backbuffer. The FPS is averaged over the last FpsAverage::kWindow seconds; the ping is WorldClient::rttMs (nullopt:
// "--"). No input and no WorldClient here: the host toggles it and passes the ping in.
namespace kast {

class FpsAverage {
public:
    static constexpr double kWindow = 0.5; // seconds

    void addFrame(double dt); // seconds since the previous frame
    int fps() const;          // frames / their summed time over the window, rounded; 0 before any frame

private:
    std::deque<double> frames_; // the newest frames' times, covering at least kWindow once there are enough
    double total_ = 0.0;
};

// A Renderer's camera pass in one frame (Renderer::stats).
struct RenderStats {
    uint64_t triangles = 0;             // submitted (instances times their triangles)
    uint32_t draws = 0;                 // draw calls: an instanced batch is one
    uint32_t occluded = 0;              // draws in the frustum that occlusion culling skipped
    std::array<uint32_t, 8> lodDraws{}; // draws (before instancing) per LOD level; terrain runs by stride level
};
// "Tris 1.23M  Draws 45  Occluded 300  LOD 12/30/40/5": levels up to the last that has draws (at least LOD0).
std::string renderStatsText(const RenderStats& stats);

// "FPS <fps>  Ping <ms|--> ms", then "  VoxelGI <ms> ms" with a VoxelGI GPU time (its volumes' compute passes) and
// "  SDFGI <ms> ms" with an SDFGI one (its cascades' compute passes), then "  " and renderStatsText if render.
std::string statsOverlayText(int fps, std::optional<int> pingMs, std::optional<double> voxelGiMs = std::nullopt,
                             std::optional<double> sdfgiMs = std::nullopt, const RenderStats* render = nullptr);
// The GPU time of the last frame's views named name (null: every view; bgfx view stats: BGFX_DEBUG_PROFILER on) whose
// id is in [first, first + count), in ms; nullopt if none drew, or a timer of theirs has no valid time this frame.
// "VoxelGI", "SDFGI": Renderer's VoxelGI and SDFGI views (renderer.h kShadowViews).
std::optional<double> viewGpuMs(const char* name, uint16_t first = 0, uint16_t count = 256);

class StatsOverlay {
public:
    // debugFlags: the host's own bgfx debug flags (e.g. BGFX_DEBUG_PROFILER), kept while BGFX_DEBUG_TEXT toggles.
    explicit StatsOverlay(uint32_t debugFlags = 0) : debugFlags_(debugFlags) {}

    void toggle(); // hidden turns bgfx debug text off again
    bool visible() const { return visible_; }
    // Every frame (it counts toward the FPS either way); prints the line while visible.
    void frame(double dt, std::optional<int> pingMs, const RenderStats* render = nullptr);

private:
    FpsAverage fps_;
    uint32_t debugFlags_;
    bool visible_ = false; // off at every start, never saved
};

} // namespace kast
