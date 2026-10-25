#include "post_process.h"

#include "fs_post_bright.bin.h"
#include "fs_post_down.bin.h"
#include "fs_post_final.bin.h"
#include "fs_post_fxaa.bin.h"
#include "fs_post_up.bin.h"
#include "fs_ssao_blur.bin.h"
#include "fs_ssao_composite.bin.h"
#include "fs_ssao_gtao.bin.h"
#include "fs_ssr_composite.bin.h"
#include "fs_ssr_trace.bin.h"
#include "fs_dof_composite.bin.h"
#include "fs_dof_gather.bin.h"
#include "fs_dof_setup.bin.h"
#include "vs_post.bin.h"

#include <bx/math.h>

#include <cmath>

namespace kast {
namespace {

constexpr uint64_t kTargetFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr uint64_t kPointClamp = BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP;
constexpr float kBloomKnee = 0.5f; // soft knee, as a fraction of the threshold
// ponytail: white balance as a red/blue gain (temperature 1 = red * 1.3, blue * 0.7); a Bradford / LMS chromatic
// adaptation if artists need real Kelvin control.
constexpr float kWhiteBalanceScale = 0.3f;

struct PostVertex {
    float x, y, z;
    float u, v;
};

bgfx::ProgramHandle postProgram(const uint8_t* fs, uint32_t fsSize)
{
    return bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_post, sizeof(vs_post))),
                               bgfx::createShader(bgfx::makeRef(fs, fsSize)), true);
}

bgfx::FrameBufferHandle colorTarget(uint16_t width, uint16_t height, bgfx::TextureFormat::Enum format)
{
    return bgfx::createFrameBuffer(width, height, format, kTargetFlags);
}

} // namespace

float exposureScale(float ev) { return std::exp2(ev); }

float acesTonemap(float x)
{
    return bx::clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

float fogFactor(float distance, float density, float start)
{
    const float d = density * bx::max(distance - start, 0.0f);
    return 1.0f - std::exp(-d * d);
}

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bx::Vec3 sceneClearColor(const ScenePostProcessing& post, uint32_t background)
{
    if (post.fog.enabled)
        return post.fog.color;
    const auto channel = [&](int shift) { return srgbToLinear(static_cast<float>((background >> shift) & 0xff) / 255.0f); };
    return {channel(24), channel(16), channel(8)};
}

std::array<float, 2> octEncode(const bx::Vec3& n)
{
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    float x = n.x / l1, y = n.y / l1;
    if (n.z < 0.0f) {
        const float wx = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float wy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = wx;
        y = wy;
    }
    return {x * 0.5f + 0.5f, y * 0.5f + 0.5f};
}

bx::Vec3 octDecode(float u, float v)
{
    const float x = u * 2.0f - 1.0f, y = v * 2.0f - 1.0f;
    bx::Vec3 n{x, y, 1.0f - std::abs(x) - std::abs(y)};
    const float t = bx::clamp(-n.z, 0.0f, 1.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return bx::normalize(n);
}

bool needsGData(const ScenePostProcessing& post, bool effects)
{
    return effects && (post.ssao.enabled || post.ssr.enabled);
}

bool needsScreenSpace(const ScenePostProcessing& post, bool effects)
{
    return needsGData(post, effects) || (effects && post.volumetricFog.enabled);
}

bx::Vec3 aoComposite(const bx::Vec3& color, const bx::Vec3& indirectDiffuse, float visibility, float intensity,
                     float power)
{
    const float occlusion = bx::clamp(intensity * (1.0f - std::pow(bx::clamp(visibility, 0.0f, 1.0f), power)), 0.0f, 1.0f);
    return bx::sub(color, bx::mul(indirectDiffuse, occlusion));
}

float gtaoSliceVisibility(float n, float h0, float h1)
{
    const float cosN = std::cos(n), sinN = std::sin(n);
    return 0.25f * (cosN + 2.0f * h0 * sinN - std::cos(2.0f * h0 - n)) +
           0.25f * (cosN + 2.0f * h1 * sinN - std::cos(2.0f * h1 - n));
}

bx::Vec3 ssrComposite(const bx::Vec3& color, const bx::Vec3& indirectSpecular, const bx::Vec3& specularWeight,
                      const bx::Vec3& reflection, float confidence, float intensity)
{
    const bx::Vec3 change = bx::sub(bx::mul(specularWeight, reflection), indirectSpecular);
    return bx::add(color, bx::mul(change, intensity * confidence));
}

float dofCocScale(const ScenePostProcessing::DepthOfField& dof, float imageHeight)
{
    constexpr float kSensorHeight = 24.0f; // mm, 35 mm full frame
    const float f = dof.focalLength, focus = dof.focusDistance * 1000.0f;
    return f * f / (dof.fStop * bx::max(focus - f, 1e-3f)) * 0.5f * imageHeight / kSensorHeight;
}

float circleOfConfusion(const ScenePostProcessing::DepthOfField& dof, float imageHeight, float distance)
{
    return dofCocScale(dof, imageHeight) * (distance - dof.focusDistance) / distance;
}

void PostProcess::init()
{
    down_.fill(BGFX_INVALID_HANDLE);
    up_.fill(BGFX_INVALID_HANDLE);
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    // One triangle past the screen's corners; uv (0, 0) is the image's first row in memory.
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    const float bottom = bottomLeft ? 0.0f : 1.0f, top = bottomLeft ? 2.0f : -1.0f; // v at y = -1 and y = 3
    const PostVertex triangle[3] = {{-1.0f, -1.0f, 0.0f, 0.0f, bottom}, {3.0f, -1.0f, 0.0f, 2.0f, bottom},
                                    {-1.0f, 3.0f, 0.0f, 0.0f, top}};
    triangle_ = bgfx::createVertexBuffer(bgfx::copy(triangle, sizeof(triangle)), layout);
    bright_ = postProgram(fs_post_bright, sizeof(fs_post_bright));
    downsample_ = postProgram(fs_post_down, sizeof(fs_post_down));
    upsample_ = postProgram(fs_post_up, sizeof(fs_post_up));
    final_ = postProgram(fs_post_final, sizeof(fs_post_final));
    fxaa_ = postProgram(fs_post_fxaa, sizeof(fs_post_fxaa));
    gtao_ = postProgram(fs_ssao_gtao, sizeof(fs_ssao_gtao));
    ssaoBlur_ = postProgram(fs_ssao_blur, sizeof(fs_ssao_blur));
    ssaoComposite_ = postProgram(fs_ssao_composite, sizeof(fs_ssao_composite));
    ssrTrace_ = postProgram(fs_ssr_trace, sizeof(fs_ssr_trace));
    ssrComposite_ = postProgram(fs_ssr_composite, sizeof(fs_ssr_composite));
    dofSetup_ = postProgram(fs_dof_setup, sizeof(fs_dof_setup));
    dofGather_ = postProgram(fs_dof_gather, sizeof(fs_dof_gather));
    dofComposite_ = postProgram(fs_dof_composite, sizeof(fs_dof_composite));
    tex0_ = bgfx::createUniform("s_tex0", bgfx::UniformType::Sampler);
    tex1_ = bgfx::createUniform("s_tex1", bgfx::UniformType::Sampler);
    texel_ = bgfx::createUniform("u_texel", bgfx::UniformType::Vec4);
    bloom_ = bgfx::createUniform("u_bloom", bgfx::UniformType::Vec4);
    post_ = bgfx::createUniform("u_post", bgfx::UniformType::Vec4);
    grade_ = bgfx::createUniform("u_grade", bgfx::UniformType::Vec4);
    grade2_ = bgfx::createUniform("u_grade2", bgfx::UniformType::Vec4);
    tex2_ = bgfx::createUniform("s_tex2", bgfx::UniformType::Sampler);
    tex3_ = bgfx::createUniform("s_tex3", bgfx::UniformType::Sampler);
    ssaoParams_ = bgfx::createUniform("u_ssao", bgfx::UniformType::Vec4);
    depthParams_ = bgfx::createUniform("u_depthParams", bgfx::UniformType::Vec4);
    ssrParams_ = bgfx::createUniform("u_ssr", bgfx::UniformType::Vec4);
    tex4_ = bgfx::createUniform("s_tex4", bgfx::UniformType::Sampler);
    dofParams_ = bgfx::createUniform("u_dof", bgfx::UniformType::Vec4);
}

void PostProcess::shutdown()
{
    destroyTargets();
    bgfx::destroy(triangle_);
    for (bgfx::ProgramHandle program : {bright_, downsample_, upsample_, final_, fxaa_, gtao_, ssaoBlur_, ssaoComposite_,
                                        ssrTrace_, ssrComposite_, dofSetup_, dofGather_, dofComposite_})
        bgfx::destroy(program);
    for (bgfx::UniformHandle uniform : {tex0_, tex1_, texel_, bloom_, post_, grade_, grade2_, tex2_, tex3_, ssaoParams_,
                                        depthParams_, ssrParams_, tex4_, dofParams_})
        bgfx::destroy(uniform);
}

void PostProcess::destroyGData()
{
    destroySsao();
    destroySsr();
    for (bgfx::FrameBufferHandle* fb : {&gdataTarget_, &sceneColor_}) {
        if (bgfx::isValid(*fb))
            bgfx::destroy(*fb);
        *fb = BGFX_INVALID_HANDLE;
    }
    for (bgfx::TextureHandle& texture : gdata_) {
        if (bgfx::isValid(texture))
            bgfx::destroy(texture);
        texture = BGFX_INVALID_HANDLE;
    }
}

void PostProcess::destroySsao()
{
    for (bgfx::FrameBufferHandle& fb : ssao_) {
        if (bgfx::isValid(fb))
            bgfx::destroy(fb);
        fb = BGFX_INVALID_HANDLE;
    }
}

void PostProcess::destroySsr()
{
    if (bgfx::isValid(ssr_))
        bgfx::destroy(ssr_);
    ssr_ = BGFX_INVALID_HANDLE;
}

void PostProcess::destroyDof()
{
    for (bgfx::FrameBufferHandle& fb : dof_) {
        if (bgfx::isValid(fb))
            bgfx::destroy(fb);
        fb = BGFX_INVALID_HANDLE;
    }
}

void PostProcess::destroyTargets()
{
    destroyDof();
    const auto destroy = [](bgfx::FrameBufferHandle& fb) {
        if (bgfx::isValid(fb))
            bgfx::destroy(fb);
        fb = BGFX_INVALID_HANDLE;
    };
    destroyGData(); // before scene_: it shares its textures
    destroy(scene_); // destroys depth_ with it
    depth_ = BGFX_INVALID_HANDLE;
    destroy(ldr_);
    for (auto& fb : down_)
        destroy(fb);
    for (auto& fb : up_)
        destroy(fb);
}

void PostProcess::begin(uint16_t width, uint16_t height, const bx::Vec3& clearColor, const ScenePostProcessing& post,
                        bool effects)
{
    if ((width != width_ || height != height_) && width > 0 && height > 0) {
        destroyTargets();
        // Not write-only: Renderer::render copies it for DEPTH_TEXTURE.
        depth_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
        const bgfx::TextureHandle textures[] = {
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F, kTargetFlags), depth_};
        scene_ = bgfx::createFrameBuffer(2, textures, true);
        ldr_ = colorTarget(width, height, bgfx::TextureFormat::BGRA8);
        // ponytail: bloom targets always exist (~1/3 of the scene target's memory); create them on first use if
        // memory gets tight.
        for (int i = 0; i < kBloomLevels; ++i) {
            sizes_[i] = {static_cast<uint16_t>(bx::max(width >> (i + 1), 1)),
                         static_cast<uint16_t>(bx::max(height >> (i + 1), 1))};
            down_[i] = colorTarget(sizes_[i].first, sizes_[i].second, bgfx::TextureFormat::RGBA16F);
            if (i < kBloomLevels - 1)
                up_[i] = colorTarget(sizes_[i].first, sizes_[i].second, bgfx::TextureFormat::RGBA16F);
        }
        width_ = width;
        height_ = height;
    }
    screenSpace_ = false;
    volumetricFog_ = false;
    if (!valid())
        return; // never sized: nothing to draw into
    const bool gdata = needsGData(post, effects) || debugGData_ != 0;
    const bool weight = gdata && effects && post.ssr.enabled; // SSR's specular weight attachment
    if (bgfx::isValid(gdataTarget_) && (!gdata || bgfx::isValid(gdata_[3]) != weight))
        destroyGData();
    if (gdata && !bgfx::isValid(gdataTarget_)) {
        gdata_[0] = bgfx::createTexture2D(width_, height_, false, 1, bgfx::TextureFormat::RGB10A2, kTargetFlags);
        for (int i = 1; i < 3; ++i)
            gdata_[i] = bgfx::createTexture2D(width_, height_, false, 1, bgfx::TextureFormat::RG11B10F, kTargetFlags);
        if (weight)
            gdata_[3] = bgfx::createTexture2D(width_, height_, false, 1, bgfx::TextureFormat::RGBA8, kTargetFlags);
        const bgfx::TextureHandle textures[] = {bgfx::getTexture(scene_), gdata_[0], gdata_[1], gdata_[2],
                                                weight ? gdata_[3] : depth_, depth_};
        const uint8_t count = weight ? 6 : 5;
        gdataTarget_ = bgfx::createFrameBuffer(count, textures, false);
        sceneColor_ = bgfx::createFrameBuffer(1, textures, false);
    }
    screenSpace_ = gdata || needsScreenSpace(post, effects);
    volumetricFog_ = effects && post.volumetricFog.enabled && debugGData_ == 0;
    // ponytail: the float clears go through bgfx's 16-entry global palette, one entry per first view for the color and
    // the next for the G-data's 0 (no surface); fine while hosts use fewer than 8 PostProcesses whose first views mod
    // 16 are at least 2 apart.
    const uint8_t palette = static_cast<uint8_t>(first_ % 16);
    bgfx::setPaletteColor(palette, clearColor.x, clearColor.y, clearColor.z, 1.0f);
    bgfx::setViewName(first_, "Scene HDR");
    bgfx::setViewRect(first_, 0, 0, width_, height_);
    if (gdata) {
        const uint8_t none = static_cast<uint8_t>((first_ + 1) % 16);
        bgfx::setPaletteColor(none, 0.0f, 0.0f, 0.0f, 0.0f);
        bgfx::setViewFrameBuffer(first_, gdataTarget_);
        bgfx::setViewClear(first_, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 1.0f, 0, palette, none, none, none, none);
    } else {
        bgfx::setViewFrameBuffer(first_, scene_);
        bgfx::setViewClear(first_, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 1.0f, 0, palette);
    }
    bgfx::touch(first_);
}

void PostProcess::pass(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height,
                       bgfx::ProgramHandle program, uint64_t state)
{
    bgfx::setViewFrameBuffer(view, target);
    bgfx::setViewRect(view, 0, 0, width, height);
    bgfx::setVertexBuffer(0, triangle_);
    bgfx::setState(state);
    bgfx::submit(view, program);
}

// SSAO in the first kSsaoViews screen-space views (before the blended draws): GTAO at half resolution from the depth
// and G-data normals, a bilateral blur, then the composite subtracts the occluded indirect diffuse from the scene's
// color with a reverse-subtract blend (sceneColor_: depth_ is sampled, so not attached). Deterministic: no history.
void PostProcess::applySsao(const ScenePostProcessing::Ssao& ssao)
{
    const auto halfWidth = static_cast<uint16_t>((width_ + 1) / 2), halfHeight = static_cast<uint16_t>((height_ + 1) / 2);
    if (!bgfx::isValid(ssao_[0]))
        for (bgfx::FrameBufferHandle& fb : ssao_)
            fb = colorTarget(halfWidth, halfHeight, bgfx::TextureFormat::RG16F);
    const bgfx::Caps* caps = bgfx::getCaps();
    const float params[4] = {ssao.radius, static_cast<float>(ssao.sliceCount), ssao.intensity, ssao.power};
    const float depthParams[4] = {caps->originBottomLeft ? 1.0f : 0.0f, caps->homogeneousDepth ? 1.0f : 0.0f, 0.0f, 0.0f};
    const float texel[4] = {1.0f / width_, 1.0f / height_, 1.0f / halfWidth, 1.0f / halfHeight};
    const auto view = [&](int i) {
        const auto id = static_cast<bgfx::ViewId>(first_ + 1 + i);
        bgfx::setViewTransform(id, nullptr, proj_.data());
        bgfx::setUniform(ssaoParams_, params);
        bgfx::setUniform(depthParams_, depthParams);
        bgfx::setUniform(texel_, texel);
        return id;
    };
    bgfx::setTexture(0, tex0_, depth_, kPointClamp);
    bgfx::setTexture(1, tex1_, gdata_[0], kPointClamp);
    bgfx::setViewName(first_ + 1, "SSAO GTAO");
    pass(view(0), ssao_[0], halfWidth, halfHeight, gtao_);
    bgfx::setTexture(0, tex0_, bgfx::getTexture(ssao_[0]), kPointClamp);
    bgfx::setTexture(1, tex1_, gdata_[0], kPointClamp);
    bgfx::setViewName(first_ + 2, "SSAO blur");
    pass(view(1), ssao_[1], halfWidth, halfHeight, ssaoBlur_);
    bgfx::setTexture(0, tex0_, bgfx::getTexture(ssao_[1]), kPointClamp);
    bgfx::setTexture(1, tex1_, depth_, kPointClamp);
    bgfx::setTexture(2, tex2_, gdata_[0], kPointClamp);
    bgfx::setTexture(3, tex3_, gdata_[1], kPointClamp);
    bgfx::setViewName(first_ + 3, "SSAO composite");
    pass(view(2), sceneColor_, width_, height_, ssaoComposite_,
         BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ADD | BGFX_STATE_BLEND_EQUATION_REVSUB);
}

// SSR in the kSsrViews after SSAO's (before the blended draws): the ray march at half resolution from the depth, G-data
// normals and roughness through the opaque scene's color (after SSAO), then the composite adds intensity * confidence *
// (specular weight * reflection - indirect specular) to that color (sceneColor_, an additive blend: the float target
// takes the negative change where the reflection is darker than the probe). Deterministic: no history.
// ponytail: a linear-then-binary march at half resolution with a fixed 4x4 gather (rough reflections keep a faint grain,
// thin objects can be stepped over at low steps); a Hi-Z march (the occlusion culler's cs_hiz pyramid is
// built from last frame's depth, so not reusable as is) and a temporal resolve if quality matters.
void PostProcess::applySsr(const ScenePostProcessing::Ssr& ssr)
{
    const auto halfWidth = static_cast<uint16_t>((width_ + 1) / 2), halfHeight = static_cast<uint16_t>((height_ + 1) / 2);
    if (!bgfx::isValid(ssr_))
        ssr_ = colorTarget(halfWidth, halfHeight, bgfx::TextureFormat::RGBA16F);
    const bgfx::Caps* caps = bgfx::getCaps();
    const float depthParams[4] = {caps->originBottomLeft ? 1.0f : 0.0f, caps->homogeneousDepth ? 1.0f : 0.0f, 0.0f, 0.0f};
    const float texel[4] = {1.0f / width_, 1.0f / height_, 1.0f / halfWidth, 1.0f / halfHeight};
    const auto view = [&](int i, const float* params) {
        const auto id = static_cast<bgfx::ViewId>(first_ + 1 + kSsaoViews + i);
        bgfx::setViewTransform(id, nullptr, proj_.data());
        bgfx::setUniform(ssrParams_, params);
        bgfx::setUniform(depthParams_, depthParams);
        bgfx::setUniform(texel_, texel);
        return id;
    };
    const float trace[4] = {ssr.maxDistance, ssr.thickness, ssr.maxRoughness, static_cast<float>(ssr.steps)};
    bgfx::setTexture(0, tex0_, depth_, kPointClamp);
    bgfx::setTexture(1, tex1_, gdata_[0], kPointClamp);
    bgfx::setTexture(2, tex2_, bgfx::getTexture(scene_));
    bgfx::setViewName(first_ + 1 + kSsaoViews, "SSR trace");
    pass(view(0, trace), ssr_, halfWidth, halfHeight, ssrTrace_);
    const float composite[4] = {ssr.intensity, ssr.maxRoughness, 0.0f, 0.0f};
    bgfx::setTexture(0, tex0_, bgfx::getTexture(ssr_), kPointClamp);
    bgfx::setTexture(1, tex1_, depth_, kPointClamp);
    bgfx::setTexture(2, tex2_, gdata_[0], kPointClamp);
    bgfx::setTexture(3, tex3_, gdata_[2], kPointClamp);
    bgfx::setTexture(4, tex4_, gdata_[3], kPointClamp);
    bgfx::setViewName(first_ + 2 + kSsaoViews, "SSR composite");
    pass(view(1, composite), sceneColor_, width_, height_, ssrComposite_, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ADD);
}

// Depth of field in the kDepthOfFieldViews from first (after the blended draws, before bloom, so highlights bloom
// from their bokeh discs): fs_dof_setup.sc halves the color with its thin-lens CoC, fs_dof_gather.sc gathers the near
// and far layers' disc bokeh, fs_dof_composite.sc blends them over the sharp color into dof_[2], returned for the rest
// of the chain. Manual focus, no history.
// ponytail: a fixed 64-tap Vogel disc gather (highlights smaller than the tap spacing, ~maxCoc / 8 half-res pixels,
// show the spiral's taps, and the near layer always gathers over maxCoc); a CoC tile max / dilation pass and a
// scatter pass for bright highlights if quality matters.
bgfx::TextureHandle PostProcess::applyDof(const ScenePostProcessing::DepthOfField& dof, bgfx::ViewId first)
{
    const auto halfWidth = static_cast<uint16_t>((width_ + 1) / 2), halfHeight = static_cast<uint16_t>((height_ + 1) / 2);
    if (!bgfx::isValid(dof_[0])) {
        dof_[0] = colorTarget(halfWidth, halfHeight, bgfx::TextureFormat::RGBA16F);
        const bgfx::TextureHandle layers[] = {
            bgfx::createTexture2D(halfWidth, halfHeight, false, 1, bgfx::TextureFormat::RGBA16F, kTargetFlags),
            bgfx::createTexture2D(halfWidth, halfHeight, false, 1, bgfx::TextureFormat::RGBA16F, kTargetFlags)};
        dof_[1] = bgfx::createFrameBuffer(2, layers, true);
        dof_[2] = colorTarget(width_, height_, bgfx::TextureFormat::RGBA16F);
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    const float params[4] = {dofCocScale(dof, height_), dof.focusDistance, dof.maxCoc, 0.0f};
    const float depthParams[4] = {caps->originBottomLeft ? 1.0f : 0.0f, caps->homogeneousDepth ? 1.0f : 0.0f, 0.0f, 0.0f};
    const float texel[4] = {1.0f / width_, 1.0f / height_, 1.0f / halfWidth, 1.0f / halfHeight};
    const auto view = [&](int i, const char* name) {
        const auto id = static_cast<bgfx::ViewId>(first + i);
        bgfx::setViewName(id, name);
        bgfx::setViewTransform(id, nullptr, proj_.data());
        bgfx::setUniform(dofParams_, params);
        bgfx::setUniform(depthParams_, depthParams);
        bgfx::setUniform(texel_, texel);
        return id;
    };
    bgfx::setTexture(0, tex0_, bgfx::getTexture(scene_));
    bgfx::setTexture(1, tex1_, depth_, kPointClamp);
    pass(view(0, "DoF setup"), dof_[0], halfWidth, halfHeight, dofSetup_);
    bgfx::setTexture(0, tex0_, bgfx::getTexture(dof_[0]));
    pass(view(1, "DoF gather"), dof_[1], halfWidth, halfHeight, dofGather_);
    bgfx::setTexture(0, tex0_, bgfx::getTexture(scene_), kPointClamp);
    bgfx::setTexture(1, tex1_, depth_, kPointClamp);
    bgfx::setTexture(2, tex2_, bgfx::getTexture(dof_[1], 0));
    bgfx::setTexture(3, tex3_, bgfx::getTexture(dof_[1], 1));
    pass(view(2, "DoF composite"), dof_[2], width_, height_, dofComposite_);
    return bgfx::getTexture(dof_[2]);
}

void PostProcess::apply(const ScenePostProcessing& post, bool effects, bgfx::FrameBufferHandle output)
{
    if (!valid())
        return;
    if (effects && post.ssao.enabled && bgfx::isValid(gdataTarget_))
        applySsao(post.ssao);
    else
        destroySsao();
    if (effects && post.ssr.enabled && post.ssr.intensity > 0.0f && bgfx::isValid(gdata_[3]))
        applySsr(post.ssr);
    else
        destroySsr();
    // After the scene, the screen-space views if used and the blended draws, then depth of field's if on.
    auto postFirst = static_cast<bgfx::ViewId>(blendView() + 1);
    const bool debug = debugGData_ >= 1 && debugGData_ <= 3 && bgfx::isValid(gdata_[debugGData_ - 1]);
    bgfx::TextureHandle sceneColor = debug ? gdata_[debugGData_ - 1] : bgfx::getTexture(scene_);
    if (effects && post.depthOfField.enabled && !debug) {
        sceneColor = applyDof(post.depthOfField, postFirst);
        postFirst = static_cast<bgfx::ViewId>(postFirst + kDepthOfFieldViews);
    } else {
        destroyDof();
    }
    bgfx::ViewId view = postFirst;

    // Bloom: soft-threshold to half size, blur down to 1/32 (4 bilinear taps over a 4x4 texel box), then back up,
    // each level = mix(its own downsample, 3x3 tent of the level below, scatter). up_[0] is added in the final pass.
    const bool bloom = effects && post.bloom.enabled && post.bloom.intensity > 0.0f;
    if (bloom) {
        const float texel[4] = {1.0f / width_, 1.0f / height_, 0.0f, 0.0f};
        const float params[4] = {post.bloom.threshold, post.bloom.threshold * kBloomKnee, 0.0f, 0.0f};
        bgfx::setTexture(0, tex0_, sceneColor);
        bgfx::setUniform(texel_, texel);
        bgfx::setUniform(bloom_, params);
        pass(view++, down_[0], sizes_[0].first, sizes_[0].second, bright_);
        for (int i = 1; i < kBloomLevels; ++i) {
            const float source[4] = {1.0f / sizes_[i - 1].first, 1.0f / sizes_[i - 1].second, 0.0f, 0.0f};
            bgfx::setTexture(0, tex0_, bgfx::getTexture(down_[i - 1]));
            bgfx::setUniform(texel_, source);
            pass(view++, down_[i], sizes_[i].first, sizes_[i].second, downsample_);
        }
        const float scatter = bx::lerp(0.05f, 0.95f, post.bloom.radius);
        for (int i = kBloomLevels - 2; i >= 0; --i) {
            const bgfx::FrameBufferHandle low = i == kBloomLevels - 2 ? down_[i + 1] : up_[i + 1];
            const float source[4] = {1.0f / sizes_[i + 1].first, 1.0f / sizes_[i + 1].second, scatter, 0.0f};
            bgfx::setTexture(0, tex0_, bgfx::getTexture(down_[i]));
            bgfx::setTexture(1, tex1_, bgfx::getTexture(low));
            bgfx::setUniform(texel_, source);
            pass(view++, up_[i], sizes_[i].first, sizes_[i].second, upsample_);
        }
    }

    // Final, then FXAA: after the bloom views, else right after the scene's (so a host with effects off uses only
    // first .. first + 3: the Shader Preview).
    view = bloom ? postFirst + 2 * kBloomLevels - 1 : postFirst;
    const auto& tone = post.tonemapping;
    const auto& grading = post.colorGrading;
    const auto& vignette = post.vignette;
    const bool toneOn = effects && tone.enabled;
    const bool gradingOn = effects && grading.enabled;
    const float t = grading.temperature * kWhiteBalanceScale;
    const float postParams[4] = {toneOn ? exposureScale(tone.exposure) : 1.0f,
                                 toneOn && tone.op == Tonemap::Aces ? 1.0f : 0.0f,
                                 bloom ? post.bloom.intensity : 0.0f, gradingOn ? 1.0f : 0.0f};
    const float grade[4] = {grading.colorFilter.x * (1.0f + t), grading.colorFilter.y, grading.colorFilter.z * (1.0f - t),
                            grading.contrast};
    const float grade2[4] = {grading.saturation, effects && vignette.enabled ? vignette.intensity : 0.0f,
                             vignette.smoothness, 0.0f};
    const bool fxaa = effects && post.fxaa.enabled;
    bgfx::setTexture(0, tex0_, sceneColor);
    bgfx::setTexture(1, tex1_, bloom ? bgfx::getTexture(up_[0]) : sceneColor);
    bgfx::setUniform(post_, postParams);
    bgfx::setUniform(grade_, grade);
    bgfx::setUniform(grade2_, grade2);
    bgfx::setViewName(view, "Post final");
    pass(view++, fxaa ? ldr_ : output, width_, height_, final_);
    if (fxaa) {
        const float texel[4] = {1.0f / width_, 1.0f / height_, 0.0f, 0.0f};
        bgfx::setTexture(0, tex0_, bgfx::getTexture(ldr_));
        bgfx::setUniform(texel_, texel);
        bgfx::setViewName(view, "Post FXAA");
        pass(view, output, width_, height_, fxaa_);
    }
}

} // namespace kast
