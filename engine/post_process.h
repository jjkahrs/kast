#pragma once

#include "scene.h"

#include <bgfx/bgfx.h>

#include <algorithm>
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
// Octahedral unit vector <-> 0..1 square (fs_mesh.sc kastOctEncode: the G-data's view-space normal).
std::array<float, 2> octEncode(const bx::Vec3& n);
bx::Vec3 octDecode(float u, float v);
// Whether post's effects (with effects on: PostProcess::apply's) need the opaque scene's G-data (SSAO, SSR), or the
// screen-space views before the blended draws at all (those and volumetric fog).
bool needsGData(const ScenePostProcessing& post, bool effects);
bool needsScreenSpace(const ScenePostProcessing& post, bool effects);
// SSAO's composite (fs_ssao_composite.sc): color less the indirect diffuse light AO occludes,
// color - indirectDiffuse * saturate(intensity * (1 - visibility^power)); direct light and emission are never darkened.
bx::Vec3 aoComposite(const bx::Vec3& color, const bx::Vec3& indirectDiffuse, float visibility, float intensity,
                     float power);
// GTAO's cosine-weighted visibility of one slice (fs_ssao_gtao.sc kastSliceVisibility): n the projected normal's angle
// from the view vector, h0 <= 0 <= h1 the horizon angles either side. Open horizons facing the view (0, -pi/2, pi/2): 1.
float gtaoSliceVisibility(float n, float h0, float h1);
// SSR's composite (fs_ssr_composite.sc): where a ray hits, the indirect specular is replaced by the reflected color times
// the surface's specular weight (G-data 4), by confidence: color + intensity * confidence * (specularWeight * reflection
// - indirectSpecular). Confidence 0 (a miss) leaves the probe / IBL specular already in color.
bx::Vec3 ssrComposite(const bx::Vec3& color, const bx::Vec3& indirectSpecular, const bx::Vec3& specularWeight,
                      const bx::Vec3& reflection, float confidence, float intensity);
// Depth of field's thin lens (fs_dof_*.sc dof.sh kastCoc): the signed circle of confusion radius in pixels of an image
// imageHeight pixels tall, at view depth distance (m), is dofCocScale * (distance - focusDistance) / distance (< 0 near,
// > 0 far), dofCocScale = f^2 / (N (S - f)) / 2 * imageHeight / 24 mm: f the focal length, N the f-stop, S the focus
// distance (mm), on a 35 mm full-frame sensor (24 mm tall, matched to the image's height so the blur doesn't change
// with the aspect). The focal length sets only the blur; the camera's own field of view is unchanged.
float dofCocScale(const ScenePostProcessing::DepthOfField& dof, float imageHeight);
float circleOfConfusion(const ScenePostProcessing::DepthOfField& dof, float imageHeight, float distance);

// The HDR scene target and the post chain that turns it into the displayed image. Draw the scene into sceneView()
// after begin(), then apply(). Uses kViews consecutive bgfx views from first: the scene, kScreenSpaceViews for SSAO,
// SSR, volumetric fog and their composite with the opaque scene, the scene's blended draws (blendView(), into
// target()), kDepthOfFieldViews, then bright pass, 4 downsamples, 4 upsamples (bloom), final (exposure, tonemap,
// grading, vignette, sRGB encode) and FXAA. SSAO uses the first kSsaoViews screen-space views: GTAO at half resolution,
// a bilateral blur, and the composite subtracting the occluded indirect diffuse from the scene's color. SSR the next
// kSsrViews: the ray march at half resolution and the composite replacing the indirect specular where rays hit (with
// intensity 0 neither runs). Volumetric fog (volumetric_fog.h, run by the Renderer: it needs the lights and shadow
// maps) the last kVolumetricFogViews: volumetricFogView(). Depth of field the kDepthOfFieldViews after the blended
// draws, before bloom: half-res color + CoC, the near / far disc gather at half resolution, and the full-res composite
// into its own target, which the rest of the chain then reads. Ranges of effects that are off are skipped: without any, the blended draws
// are at first + 1 and final and FXAA at first + 2 and + 3, as before these effects (a host with effects off, the
// Shader Preview, uses only first .. first + 3).
// G-data (needsGData): the scene view then draws into four color attachments, written by every opaque and mask mesh
// (fs_mesh.sc): 0 the lit HDR color (as without), 1 normalRoughness() RGB10A2: the view-space normal octahedral in rg
// (octEncode), perceptual roughness in b, a 1 (0: no surface, what the clear and the sky leave); 2 indirectDiffuse()
// and 3 indirectSpecular() RG11B10F: the ambient / IBL / probe / lightmap / VoxelGI / SDFGI light in color, diffuse
// and specular, times material AO and what distance fog leaves of it (0: none). Blended draws and particles go to
// target(), which has only the color, so they write none. With SSR also 4 specularWeight() RGBA8: the split sum's
// specular weight (f0 * A + B) times material AO and fog's keep, what SSR scales its reflection by. Made when first
// needed, freed when no longer needed or on a resize (3 x 32 bits per pixel, 4 with SSR: 24 / 32 MiB at 1920 x 1080).
class PostProcess {
public:
    static constexpr bgfx::ViewId kScreenSpaceViews = 7;
    static constexpr bgfx::ViewId kSsaoViews = 3; // the first of kScreenSpaceViews
    static constexpr bgfx::ViewId kSsrViews = 2;  // the next
    static constexpr bgfx::ViewId kVolumetricFogViews = 2; // the last: its compute passes, its composite
    static_assert(kSsaoViews + kSsrViews + kVolumetricFogViews == kScreenSpaceViews);
    static constexpr bgfx::ViewId kDepthOfFieldViews = 3;
    static constexpr bgfx::ViewId kViews = 13 + kScreenSpaceViews + kDepthOfFieldViews;

    explicit PostProcess(bgfx::ViewId first) : first_(first) {}
    void init();
    void shutdown();
    // Sizes the targets (recreated only when the size changes; not if either is 0) and sets sceneView() to draw into
    // RGBA16F color + depth, cleared to clearColor (linear) and depth 1, plus the G-data if post and effects (as
    // apply() will get them) need it. Picks blendView() the same way.
    void begin(uint16_t width, uint16_t height, const bx::Vec3& clearColor, const ScenePostProcessing& post = {},
               bool effects = false);
    bgfx::ViewId sceneView() const { return first_; }
    // Where the scene's blended draws go (Renderer::setTarget): first + 1, or after the screen-space views.
    bgfx::ViewId blendView() const { return static_cast<bgfx::ViewId>(first_ + 1 + (screenSpace_ ? kScreenSpaceViews : 0)); }
    // The first of volumetric fog's views this frame (Renderer::setTarget's fogView): with effects and the scene's
    // volumetricFog on (begin()), else UINT16_MAX.
    bgfx::ViewId volumetricFogView() const
    {
        return volumetricFog_ ? static_cast<bgfx::ViewId>(first_ + 1 + kSsaoViews + kSsrViews) : bgfx::ViewId(UINT16_MAX);
    }
    // This frame's G-data (invalid without): see the class comment.
    bgfx::TextureHandle normalRoughness() const { return gdata_[0]; }
    bgfx::TextureHandle indirectDiffuse() const { return gdata_[1]; }
    bgfx::TextureHandle indirectSpecular() const { return gdata_[2]; }
    bgfx::TextureHandle specularWeight() const { return gdata_[3]; } // with SSR only
    // Test hook (--show-gdata): 1..3 makes the G-data whatever the effects and shows its attachment 1..3 instead of
    // the scene's color (through final's sRGB encode, and whichever of its effects are on); 0 off.
    void setDebugGData(int attachment) { debugGData_ = attachment; }
    // The camera's projection the scene was drawn with (cameraMatrices), for the screen-space effects to read its depth.
    void setProjection(const float* proj) { std::copy_n(proj, 16, proj_.begin()); }
    bool valid() const { return bgfx::isValid(scene_); }
    // The scene target (RGBA16F color, D24S8 depth) and its size: Renderer::setTarget.
    bgfx::FrameBufferHandle target() const { return scene_; }
    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    // The scene's depth: attach it to output to depth-test editor overlays drawn after apply().
    bgfx::TextureHandle depth() const { return depth_; }
    // Runs the chain into output (invalid = backbuffer), same size as begin(). effects = false skips every effect
    // except the sRGB encode (the editor's post toggle); fog is not a post effect and stays.
    void apply(const ScenePostProcessing& post, bool effects, bgfx::FrameBufferHandle output);

private:
    static constexpr int kBloomLevels = 5; // half resolution down to 1/32

    void destroyTargets();
    void destroyGData();
    void destroySsao();
    void applySsao(const ScenePostProcessing::Ssao& ssao);
    void destroySsr();
    void applySsr(const ScenePostProcessing::Ssr& ssr);
    void destroyDof();
    bgfx::TextureHandle applyDof(const ScenePostProcessing::DepthOfField& dof, bgfx::ViewId first);
    void pass(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height,
              bgfx::ProgramHandle program, uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

    bgfx::ViewId first_;
    uint16_t width_ = 0, height_ = 0;
    bgfx::TextureHandle depth_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle scene_ = BGFX_INVALID_HANDLE; // RGBA16F + depth_
    std::array<bgfx::TextureHandle, 4> gdata_{
        {{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}};
    bgfx::FrameBufferHandle gdataTarget_ = BGFX_INVALID_HANDLE; // scene_'s color, gdata_, depth_ (owns none)
    bgfx::FrameBufferHandle sceneColor_ = BGFX_INVALID_HANDLE;  // scene_'s color alone, with the G-data (owns none)
    std::array<bgfx::FrameBufferHandle, 2> ssao_{{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}}; // RG16F half: raw, blurred
    bgfx::FrameBufferHandle ssr_ = BGFX_INVALID_HANDLE; // RGBA16F half: the ray march's reflections
    // RGBA16F: half color + CoC, half far + near layers (one 2-attachment target), full composite
    std::array<bgfx::FrameBufferHandle, 3> dof_{{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}};
    std::array<float, 16> proj_{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool screenSpace_ = false;
    bool volumetricFog_ = false;
    int debugGData_ = 0;
    bgfx::FrameBufferHandle ldr_ = BGFX_INVALID_HANDLE;   // BGRA8, final pass output when FXAA follows
    std::array<bgfx::FrameBufferHandle, kBloomLevels> down_;     // RGBA16F, level i at size >> (i + 1); init() invalidates
    std::array<bgfx::FrameBufferHandle, kBloomLevels - 1> up_;   // RGBA16F, same sizes as down_[0..3]
    std::array<std::pair<uint16_t, uint16_t>, kBloomLevels> sizes_{};

    bgfx::VertexBufferHandle triangle_ = BGFX_INVALID_HANDLE; // covers the screen
    bgfx::ProgramHandle bright_ = BGFX_INVALID_HANDLE, downsample_ = BGFX_INVALID_HANDLE,
                        upsample_ = BGFX_INVALID_HANDLE, final_ = BGFX_INVALID_HANDLE, fxaa_ = BGFX_INVALID_HANDLE,
                        gtao_ = BGFX_INVALID_HANDLE, ssaoBlur_ = BGFX_INVALID_HANDLE, ssaoComposite_ = BGFX_INVALID_HANDLE,
                        ssrTrace_ = BGFX_INVALID_HANDLE, ssrComposite_ = BGFX_INVALID_HANDLE,
                        dofSetup_ = BGFX_INVALID_HANDLE, dofGather_ = BGFX_INVALID_HANDLE, dofComposite_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle tex0_ = BGFX_INVALID_HANDLE, tex1_ = BGFX_INVALID_HANDLE, texel_ = BGFX_INVALID_HANDLE,
                        bloom_ = BGFX_INVALID_HANDLE, post_ = BGFX_INVALID_HANDLE, grade_ = BGFX_INVALID_HANDLE,
                        grade2_ = BGFX_INVALID_HANDLE, tex2_ = BGFX_INVALID_HANDLE, tex3_ = BGFX_INVALID_HANDLE,
                        ssaoParams_ = BGFX_INVALID_HANDLE, depthParams_ = BGFX_INVALID_HANDLE,
                        ssrParams_ = BGFX_INVALID_HANDLE, tex4_ = BGFX_INVALID_HANDLE,
                        dofParams_ = BGFX_INVALID_HANDLE;
};

} // namespace kast
