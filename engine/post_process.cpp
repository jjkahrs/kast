#include "post_process.h"

#include "fs_post_bright.bin.h"
#include "fs_post_down.bin.h"
#include "fs_post_final.bin.h"
#include "fs_post_fxaa.bin.h"
#include "fs_post_up.bin.h"
#include "vs_post.bin.h"

#include <bx/math.h>

#include <cmath>

namespace kast {
namespace {

constexpr uint64_t kTargetFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr uint64_t kPassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
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
    tex0_ = bgfx::createUniform("s_tex0", bgfx::UniformType::Sampler);
    tex1_ = bgfx::createUniform("s_tex1", bgfx::UniformType::Sampler);
    texel_ = bgfx::createUniform("u_texel", bgfx::UniformType::Vec4);
    bloom_ = bgfx::createUniform("u_bloom", bgfx::UniformType::Vec4);
    post_ = bgfx::createUniform("u_post", bgfx::UniformType::Vec4);
    grade_ = bgfx::createUniform("u_grade", bgfx::UniformType::Vec4);
    grade2_ = bgfx::createUniform("u_grade2", bgfx::UniformType::Vec4);
}

void PostProcess::shutdown()
{
    destroyTargets();
    bgfx::destroy(triangle_);
    for (bgfx::ProgramHandle program : {bright_, downsample_, upsample_, final_, fxaa_})
        bgfx::destroy(program);
    for (bgfx::UniformHandle uniform : {tex0_, tex1_, texel_, bloom_, post_, grade_, grade2_})
        bgfx::destroy(uniform);
}

void PostProcess::destroyTargets()
{
    const auto destroy = [](bgfx::FrameBufferHandle& fb) {
        if (bgfx::isValid(fb))
            bgfx::destroy(fb);
        fb = BGFX_INVALID_HANDLE;
    };
    destroy(scene_); // destroys depth_ with it
    depth_ = BGFX_INVALID_HANDLE;
    destroy(ldr_);
    for (auto& fb : down_)
        destroy(fb);
    for (auto& fb : up_)
        destroy(fb);
}

void PostProcess::begin(uint16_t width, uint16_t height, const bx::Vec3& clearColor)
{
    if ((width != width_ || height != height_) && width > 0 && height > 0) {
        destroyTargets();
        depth_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
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
    if (!valid())
        return; // never sized: nothing to draw into
    // ponytail: the float clear goes through bgfx's 16-entry global palette, one entry per first view; fine while
    // hosts use fewer than 16 PostProcesses with distinct first views mod 16.
    const uint8_t palette = static_cast<uint8_t>(first_ % 16);
    bgfx::setPaletteColor(palette, clearColor.x, clearColor.y, clearColor.z, 1.0f);
    bgfx::setViewName(first_, "Scene HDR");
    bgfx::setViewFrameBuffer(first_, scene_);
    bgfx::setViewRect(first_, 0, 0, width_, height_);
    bgfx::setViewClear(first_, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 1.0f, 0, palette);
    bgfx::touch(first_);
}

void PostProcess::pass(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height,
                       bgfx::ProgramHandle program)
{
    bgfx::setViewFrameBuffer(view, target);
    bgfx::setViewRect(view, 0, 0, width, height);
    bgfx::setVertexBuffer(0, triangle_);
    bgfx::setState(kPassState);
    bgfx::submit(view, program);
}

void PostProcess::apply(const ScenePostProcessing& post, bool effects, bgfx::FrameBufferHandle output)
{
    if (!valid())
        return;
    bgfx::ViewId view = first_ + 1;
    const bgfx::TextureHandle sceneColor = bgfx::getTexture(scene_);

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

    view = first_ + 1 + 2 * kBloomLevels - 1; // final, then FXAA, whether or not bloom ran
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
