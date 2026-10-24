#pragma once

#include "math3d.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// Hi-Z occlusion culling for the Renderer's camera pass (docs/engine/lod-and-occlusion.html). The math is GPU-free so
// self_check can test it; cs_hiz_build.sc and cs_hiz_test.sc do the same on the GPU.
namespace kast {

// Each box is grown by this fraction of its largest extent on every side (at least kOcclusionMinDilation metres), so
// results a few frames old don't drop a draw at a silhouette while the camera moves.
constexpr float kOcclusionDilation = 0.02f;
constexpr float kOcclusionMinDilation = 0.05f;
// Results tested from an eye further than this from the camera's (a cut, a teleport, a fast move) are dropped:
// everything draws until a test from near the new eye comes back.
constexpr float kOcclusionMaxEyeMove = 2.0f;
// Frames with fewer candidates than this test nothing: the pyramid and read-back cost a fixed few tens of
// microseconds a frame, more than culling a handful of draws saves.
constexpr size_t kOcclusionMinDraws = 64;
// While the newest result hid under this fraction of the draws it tested, a test runs only every
// kOcclusionIdleInterval frames (its results stand meanwhile): the pyramid, test and read-back cost about 0.1 ms a
// frame, more than they save where little is hidden.
constexpr float kOcclusionUsefulFraction = 0.2f;
constexpr uint64_t kOcclusionIdleInterval = 8;
// Draws tested per frame (the visibility image is kOcclusionImageWidth wide); the rest always draw.
constexpr uint32_t kOcclusionImageWidth = 256;
constexpr uint32_t kOcclusionMaxDraws = kOcclusionImageWidth * kOcclusionImageWidth;

// box grown by kOcclusionDilation of its largest extent (at least kOcclusionMinDilation) on every side.
Aabb dilateForOcclusion(const Aabb& box);
// True if inner lies inside outer (faces may touch).
bool contains(const Aabb& outer, const Aabb& inner);

// The size of the Hi-Z pyramid's mip 0 for a depth target: per axis the largest power of 2 not over half of it (at
// least 1; 1280 x 720 -> 512 x 256), so every mip below halves exactly and mip 0 stays cheap to build. hizMipCount:
// mips down to 1 x 1.
std::pair<uint16_t, uint16_t> hizSize(uint16_t width, uint16_t height);
uint8_t hizMipCount(uint16_t width0, uint16_t height0);
// The depth pixels [x0, x1] (inclusive) that mip-0 texel x of a width0-wide pyramid covers over a width-wide depth
// target: up to 5, partly covered ones included, so the max over them is conservative.
std::pair<uint32_t, uint32_t> hizDepthSpan(uint32_t x, uint32_t width, uint32_t width0);

// A box's screen rect through viewProj: u, v 0..1 from the top-left (Direct3D: no flip unless originBottomLeft) and
// its nearest depth 0..1.
struct HizRect {
    float u0, v0, u1, v1;
    float depth;
};
// nullopt if a corner is behind the eye or nearer than the near plane: such a box is always visible.
std::optional<HizRect> hizProject(const Aabb& box, const float* viewProj, bool homogeneousDepth, bool originBottomLeft);

// The texels to read for a rect: the mip at which it spans at most 2 x 2 texels, and those texels (inclusive).
struct HizFootprint {
    int mip;
    int x0, y0, x1, y1;
};
HizFootprint hizFootprint(const HizRect& rect, uint16_t width0, uint16_t height0, uint8_t mips);

// A Hi-Z pyramid on the CPU, as the compute shaders build it (for self_check): mips[0] is width0 x height0, each
// texel the farthest depth under it.
struct HizPyramid {
    uint16_t width0 = 0, height0 = 0;
    std::vector<std::vector<float>> mips;
};
HizPyramid buildHizPyramid(std::span<const float> depth, uint16_t width, uint16_t height);
// True if the box's nearest depth is beyond the farthest depth in its footprint: it is hidden behind what drew.
bool hizOccluded(const HizPyramid& pyramid, const Aabb& box, const float* viewProj, bool homogeneousDepth,
                 bool originBottomLeft);

// The GPU side, one per Renderer. Each frame the Renderer calls beginFrame, asks occluded() for each draw that passed
// the frustum, then endFrame with the ones it tested: that copies this frame's depth, and the next frame builds a Hi-Z
// pyramid from it with compute and tests these boxes against it (cs_hiz_test.sc), the visibility read back 3 frames
// after that. So a result is 5 frames old when used: a draw that comes into view from behind an occluder
// pops in that late while the camera moves (box dilation hides most of it). With no compute, blit or read-back
// (bgfx::getCaps), or forced off (setForceUnsupported), nothing is ever occluded.
class HizOcclusion {
public:
    struct Candidate {
        uint64_t id; // the draw's stable identity across frames (never 0)
        Aabb box;    // world space
    };

    void init();
    void shutdown();
    // Test hook (--occlusion-unsupported): behave as on a GPU without compute / read-back. Set before init.
    void setForceUnsupported(bool on) { forceUnsupported_ = on; }
    bool supported() const { return supported_; }
    // A frame of a width x height depth target seen from eye: takes in finished read-backs. A resize drops every
    // result; so does an eye kOcclusionMaxEyeMove from the one the results were tested from.
    void beginFrame(const bx::Vec3& eye, uint16_t width, uint16_t height);
    // The last result for draw id says it was hidden, and bounds still lies in the box it was tested with.
    bool occluded(uint64_t id, const Aabb& bounds) const;
    // Tests last frame's candidates (computes in view, before its draws; the read-back blit in copyView), then copies
    // depth (the target's, as view drew it; blitted at copyView's start) and keeps candidates (boxes dilated here;
    // none if under kOcclusionMinDraws) and viewProj for the next frame.
    void endFrame(bgfx::ViewId view, bgfx::ViewId copyView, bgfx::TextureHandle depth, const float* viewProj,
                  const bx::Vec3& eye, std::vector<Candidate> candidates);
    // Drops every result and pending test: everything draws until new ones come back.
    void reset();
    // Frames from a depth copy to the frame its results were first used, the last time one came back; 0: none yet.
    int latency() const { return latency_; }
    // Debug view: an RGBA8 picture of the pyramid's mip (clamped to the last; -1 off), refreshed when a test runs.
    void setDebugMip(int mip) { debugMip_ = mip; }
    bgfx::TextureHandle debugTexture() const { return debugMip_ >= 0 ? debug_ : bgfx::TextureHandle BGFX_INVALID_HANDLE; }

private:
    struct Pending {
        bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
        std::vector<float> data;
        std::vector<Candidate> candidates;
        bx::Vec3 eye{0.0f, 0.0f, 0.0f};
        uint64_t depthFrame = 0, blitFrame = 0;
        bool waiting = false;   // tested, until its result is taken in
        bool requested = false; // bgfx::readTexture asked (the frame after the blit)
    };
    void destroyTargets();
    void destroyVisibility();

    bool forceUnsupported_ = false, supported_ = false;
    bgfx::ProgramHandle buildProgram_ = BGFX_INVALID_HANDLE, testProgram_ = BGFX_INVALID_HANDLE,
                        debugProgram_ = BGFX_INVALID_HANDLE;
    // 1 x 1 R32F bound to the image stages a pass doesn't use: one view bound to two stages breaks the dispatch.
    std::array<bgfx::TextureHandle, 4> spares_{};
    bgfx::UniformHandle params_ = BGFX_INVALID_HANDLE, viewProj_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle depthSampler_ = BGFX_INVALID_HANDLE, pyramidSampler_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout boxLayout_;
    bgfx::DynamicVertexBufferHandle boxes_ = BGFX_INVALID_HANDLE; // per candidate: min, max (vec4 each)
    // Per depth target size: its copy, and the pyramid (hizSize, R32F, mips).
    uint16_t width_ = 0, height_ = 0, width0_ = 0, height0_ = 0;
    uint8_t mips_ = 0;
    bgfx::TextureHandle depthCopy_ = BGFX_INVALID_HANDLE, pyramid_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle debug_ = BGFX_INVALID_HANDLE;
    int debugMip_ = -1, debugBuilt_ = -1;
    // Visibility: 1 visible / 0 hidden per candidate, kOcclusionImageWidth x rows_ (R32F), read back via pending_.
    uint32_t rows_ = 0;
    bgfx::TextureHandle visibility_ = BGFX_INVALID_HANDLE;
    std::array<Pending, 6> pending_;
    // Last frame's: candidates, viewProj, eye, and whether depthCopy_ holds its depth.
    std::vector<Candidate> staged_;
    float stagedViewProj_[16] = {};
    bx::Vec3 stagedEye_{0.0f, 0.0f, 0.0f};
    uint64_t stagedFrame_ = 0;
    // The newest result: the boxes found hidden, by draw id, and the eye they were tested from.
    std::unordered_map<uint64_t, Aabb> hidden_;
    bx::Vec3 resultEye_{0.0f, 0.0f, 0.0f};
    uint64_t frame_ = 0;     // beginFrame calls
    uint64_t nextStage_ = 0; // the next frame to keep candidates (and copy depth) for a test
    bool useful_ = true;     // the newest result hid at least kOcclusionUsefulFraction
    int latency_ = 0;
};

} // namespace kast
