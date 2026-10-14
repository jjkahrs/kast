#pragma once

#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <utility>

namespace kast {

// CPU mirrors of the post and fog shader math, for self_check. Keep in sync with the shaders named.
// 2^ev (fs_post_final.sc u_post.x).
float exposureScale(float ev);
// ACES filmic fit (Narkowicz 2015): saturate(x(2.51x + 0.03) / (x(2.43x + 0.59) + 0.14)) (fs_post_final.sc aces).
float acesTonemap(float x);
// Exponential-squared fog: 1 - exp(-(density * max(d - start, 0))^2); 0 = no fog, toward 1 = all fog (fs_mesh.sc).
float fogFactor(float distance, float density, float start);
// sRGB (IEC 61966-2-1, the inverse of fs_post_final.sc toSrgb) to linear, per channel 0..1.
float srgbToLinear(float c);

// What a scene view clears to (linear RGB): the fog color while fog is on, else background (0xRRGGBBAA, sRGB).
bx::Vec3 sceneClearColor(const ScenePostProcessing& post, uint32_t background);

// The HDR scene target and the post chain that turns it into the displayed image. Draw the scene into sceneView()
// after begin(), then apply(). Uses kViews consecutive bgfx views from first: the scene, then bright pass, 4
// downsamples, 4 upsamples (bloom), final (exposure, tonemap, grading, vignette, sRGB encode) and FXAA.
class PostProcess {
public:
    static constexpr bgfx::ViewId kViews = 12;

    explicit PostProcess(bgfx::ViewId first) : first_(first) {}
    void init();
    void shutdown();
    // Sizes the targets (recreated only when the size changes; not if either is 0) and sets sceneView() to draw into
    // RGBA16F color + depth, cleared to clearColor (linear) and depth 1.
    void begin(uint16_t width, uint16_t height, const bx::Vec3& clearColor);
    bgfx::ViewId sceneView() const { return first_; }
    bool valid() const { return bgfx::isValid(scene_); }
    // The scene's depth: attach it to output to depth-test editor overlays drawn after apply().
    bgfx::TextureHandle depth() const { return depth_; }
    // Runs the chain into output (invalid = backbuffer), same size as begin(). effects = false skips every effect
    // except the sRGB encode (the editor's post toggle); fog is not a post effect and stays.
    void apply(const ScenePostProcessing& post, bool effects, bgfx::FrameBufferHandle output);

private:
    static constexpr int kBloomLevels = 5; // half resolution down to 1/32

    void destroyTargets();
    void pass(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height,
              bgfx::ProgramHandle program);

    bgfx::ViewId first_;
    uint16_t width_ = 0, height_ = 0;
    bgfx::TextureHandle depth_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle scene_ = BGFX_INVALID_HANDLE; // RGBA16F + depth_
    bgfx::FrameBufferHandle ldr_ = BGFX_INVALID_HANDLE;   // BGRA8, final pass output when FXAA follows
    std::array<bgfx::FrameBufferHandle, kBloomLevels> down_;     // RGBA16F, level i at size >> (i + 1); init() invalidates
    std::array<bgfx::FrameBufferHandle, kBloomLevels - 1> up_;   // RGBA16F, same sizes as down_[0..3]
    std::array<std::pair<uint16_t, uint16_t>, kBloomLevels> sizes_{};

    bgfx::VertexBufferHandle triangle_ = BGFX_INVALID_HANDLE; // covers the screen
    bgfx::ProgramHandle bright_ = BGFX_INVALID_HANDLE, downsample_ = BGFX_INVALID_HANDLE,
                        upsample_ = BGFX_INVALID_HANDLE, final_ = BGFX_INVALID_HANDLE, fxaa_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle tex0_ = BGFX_INVALID_HANDLE, tex1_ = BGFX_INVALID_HANDLE, texel_ = BGFX_INVALID_HANDLE,
                        bloom_ = BGFX_INVALID_HANDLE, post_ = BGFX_INVALID_HANDLE, grade_ = BGFX_INVALID_HANDLE,
                        grade2_ = BGFX_INVALID_HANDLE;
};

} // namespace kast
