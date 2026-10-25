#include "volumetric_fog.h"

#include "cs_volfog_inject.bin.h"
#include "cs_volfog_integrate.bin.h"
#include "cs_volfog_scatter.bin.h"
#include "fs_volfog_composite.bin.h"
#include "vs_post.bin.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>

namespace kast {
namespace {

constexpr uint64_t kVolumeFlags = BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_UVW_CLAMP;
constexpr uint64_t kShadowFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | BGFX_SAMPLER_UVW_CLAMP;
constexpr uint16_t kGridUniforms = 12; // u_volFogGrid in the cs_volfog_*.sc shaders

struct PostVertex {
    float x, y, z;
    float u, v;
};

bgfx::ProgramHandle computeProgram(const uint8_t* cs, uint32_t size)
{
    return bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs, size)), true);
}

bgfx::TextureHandle volume()
{
    return bgfx::createTexture3D(kFroxelsX, kFroxelsY, kFroxelSlices, false, bgfx::TextureFormat::RGBA16F, kVolumeFlags);
}

} // namespace

float henyeyGreenstein(float cosTheta, float g)
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * bx::kPi * std::pow(std::max(1.0f + g2 - 2.0f * g * cosTheta, 1e-6f), 1.5f));
}

float froxelSliceDepth(float slice, float maxDistance)
{
    return kFroxelNear * std::pow(maxDistance / kFroxelNear, slice / float(kFroxelSlices));
}

float froxelSlice(float depth, float maxDistance)
{
    return float(kFroxelSlices) * std::log(std::max(depth, 1e-4f) / kFroxelNear) / std::log(maxDistance / kFroxelNear);
}

FogVolumeSample fogVolumeSample(const FogVolume& volume, const float* world)
{
    FogVolumeSample out;
    bx::mtxInverse(out.worldToLocal.data(), world);
    out.shape = volume.shape;
    out.density = volume.density;
    out.edgeFalloff = volume.edgeFalloff;
    out.heightFalloff = volume.heightFalloff;
    out.albedo = volume.albedo;
    bx::Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int i = 0; i < 8; ++i) { // the unit box's corners (the sphere is inside it)
        const bx::Vec3 corner = bx::mul(bx::Vec3{i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f, i & 4 ? 0.5f : -0.5f}, world);
        lo = bx::min(lo, corner);
        hi = bx::max(hi, corner);
    }
    out.bounds = {lo, hi};
    out.bottom = lo.y;
    return out;
}

float fogVolumeDensity(const FogVolumeSample& volume, const bx::Vec3& p)
{
    const bx::Vec3 q = bx::mul(p, volume.worldToLocal.data());
    const float d = 2.0f * (volume.shape == FogVolumeShape::Box
                                ? std::max({std::abs(q.x), std::abs(q.y), std::abs(q.z)})
                                : bx::length(q));
    if (d >= 1.0f)
        return 0.0f;
    const float edge = volume.edgeFalloff > 0.0f ? std::clamp((1.0f - d) / volume.edgeFalloff, 0.0f, 1.0f) : 1.0f;
    return volume.density * edge * std::exp(-volume.heightFalloff * std::max(p.y - volume.bottom, 0.0f));
}

void VolumetricFog::init()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    constexpr uint16_t kImage3D =
        BGFX_CAPS_FORMAT_TEXTURE_3D | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_READ;
    if (!(caps->supported & BGFX_CAPS_COMPUTE) || !(caps->supported & BGFX_CAPS_TEXTURE_3D) ||
        (caps->formats[bgfx::TextureFormat::RGBA16F] & kImage3D) != kImage3D)
        return; // warned when a scene turns it on (Renderer::render)
    inject_ = computeProgram(cs_volfog_inject, sizeof(cs_volfog_inject));
    scatter_ = computeProgram(cs_volfog_scatter, sizeof(cs_volfog_scatter));
    integrate_ = computeProgram(cs_volfog_integrate, sizeof(cs_volfog_integrate));
    composite_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_post, sizeof(vs_post))),
                                     bgfx::createShader(bgfx::makeRef(fs_volfog_composite, sizeof(fs_volfog_composite))),
                                     true);
    grid_ = bgfx::createUniform("u_volFogGrid", bgfx::UniformType::Vec4, kGridUniforms);
    prev_ = bgfx::createUniform("u_volFogPrev", bgfx::UniformType::Mat4);
    lights_ = bgfx::createUniform("u_volFogLights", bgfx::UniformType::Vec4, kMaxVoxelGiLights * 4);
    volumeMtx_ = bgfx::createUniform("u_fogVolumeMtx", bgfx::UniformType::Mat4, kMaxFogVolumes);
    volumes_ = bgfx::createUniform("u_fogVolumes", bgfx::UniformType::Vec4, kMaxFogVolumes * 2);
    csmMtx_ = bgfx::createUniform("u_csmMtx", bgfx::UniformType::Mat4, kMaxShadowCascades + kMaxShadowedLocalLights);
    csmBias_ = bgfx::createUniform("u_csmBias", bgfx::UniformType::Vec4, kMaxShadowCascades);
    csmSampler_ = bgfx::createUniform("s_csm", bgfx::UniformType::Sampler);
    for (size_t i = 0; i < kMaxShadowedPointLights; ++i)
        cubeSamplers_[i] = bgfx::createUniform(std::format("s_pointShadow{}", i).c_str(), bgfx::UniformType::Sampler);
    historySampler_ = bgfx::createUniform("s_history", bgfx::UniformType::Sampler);
    drawUniform_ = bgfx::createUniform("u_volFog", bgfx::UniformType::Vec4, 2);
    tex0_ = bgfx::createUniform("s_tex0", bgfx::UniformType::Sampler);
    tex1_ = bgfx::createUniform("s_tex1", bgfx::UniformType::Sampler);
    // 1x1 stand-ins for the shadow maps not made yet: D3D11 wants a comparison sampler where the shader declares one.
    noShadow2D_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::D16, kShadowFlags);
    noShadowCube_ = bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::D16, kShadowFlags);
    // The composite's triangle past the screen's corners, as PostProcess's: uv (0, 0) is the image's first row in memory.
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    const float bottom = caps->originBottomLeft ? 0.0f : 1.0f, top = caps->originBottomLeft ? 2.0f : -1.0f;
    const PostVertex triangle[3] = {{-1.0f, -1.0f, 0.0f, 0.0f, bottom}, {3.0f, -1.0f, 0.0f, 2.0f, bottom},
                                    {-1.0f, 3.0f, 0.0f, 0.0f, top}};
    triangle_ = bgfx::createVertexBuffer(bgfx::copy(triangle, sizeof(triangle)), layout);
}

void VolumetricFog::shutdown()
{
    release();
    if (!supported())
        return;
    for (bgfx::ProgramHandle program : {inject_, scatter_, integrate_, composite_})
        bgfx::destroy(program);
    for (bgfx::UniformHandle uniform : {grid_, prev_, lights_, volumeMtx_, volumes_, csmMtx_, csmBias_, csmSampler_,
                                        historySampler_, drawUniform_, tex0_, tex1_})
        bgfx::destroy(uniform);
    for (bgfx::UniformHandle uniform : cubeSamplers_)
        bgfx::destroy(uniform);
    bgfx::destroy(noShadow2D_);
    bgfx::destroy(noShadowCube_);
    bgfx::destroy(triangle_);
    inject_ = BGFX_INVALID_HANDLE;
}

void VolumetricFog::release()
{
    if (bgfx::isValid(color_))
        bgfx::destroy(color_);
    color_ = BGFX_INVALID_HANDLE;
    colorTexture_ = BGFX_INVALID_HANDLE;
    for (bgfx::TextureHandle* texture : {&density_, &integrated_, &scattering_[0], &scattering_[1]}) {
        if (bgfx::isValid(*texture))
            bgfx::destroy(*texture);
        *texture = BGFX_INVALID_HANDLE;
    }
    history_ = false;
}

bool VolumetricFog::update(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height,
                           const Frame& frame)
{
    if (!supported())
        return false;
    const ScenePostProcessing::VolumetricFog& s = frame.settings;
    const bgfx::Caps* caps = bgfx::getCaps();
    float viewProj[16], invViewProj[16], invView[16];
    bx::mtxMul(viewProj, frame.viewMtx, frame.projMtx);
    bx::mtxInverse(invViewProj, viewProj);
    bx::mtxInverse(invView, frame.viewMtx);
    const bx::Vec3 eye{invView[12], invView[13], invView[14]};
    // Per meter of view depth, the ray through a point of the screen (NDC), from where it leaves the eye's plane: the
    // eye for a perspective projection, a point beside it for an orthographic one (parallel rays). Froxel row 0 is the
    // image's first row in memory, as the samplers' uv.
    const bool ortho = frame.projMtx[15] != 0.0f;
    const auto ray = [&](float x, float y) { return bx::sub(bx::mulH(bx::Vec3{x, y, 0.5f}, invViewProj), eye); };
    const bx::Vec3 forward = bx::normalize(ray(0.0f, 0.0f));
    const auto unit = [&](float x, float y) {
        const bx::Vec3 r = ray(x, y);
        return ortho ? forward : bx::mul(r, 1.0f / bx::dot(r, forward));
    };
    const auto start = [&](float x, float y) {
        const bx::Vec3 r = ray(x, y);
        return ortho ? bx::add(eye, bx::sub(r, bx::mul(forward, bx::dot(r, forward)))) : eye;
    };
    const float firstRow = caps->originBottomLeft ? -1.0f : 1.0f;
    const bx::Vec3 topLeft = unit(-1.0f, firstRow);
    const bx::Vec3 right = bx::sub(unit(1.0f, firstRow), topLeft), down = bx::sub(unit(-1.0f, -firstRow), topLeft);
    const bx::Vec3 topLeftStart = start(-1.0f, firstRow);
    const bx::Vec3 startRight = bx::sub(start(1.0f, firstRow), topLeftStart);
    const bx::Vec3 startDown = bx::sub(start(-1.0f, -firstRow), topLeftStart);
    Aabb bounds{eye, eye}; // the froxel grid's
    for (int i = 0; i < 4; ++i) {
        const auto corner = [&](const bx::Vec3& first, const bx::Vec3& across, const bx::Vec3& below) {
            return bx::add(bx::add(first, bx::mul(across, float(i & 1))), bx::mul(below, float(i / 2)));
        };
        const bx::Vec3 near = corner(topLeftStart, startRight, startDown);
        const bx::Vec3 far = bx::add(near, bx::mul(corner(topLeft, right, down), s.maxDistance));
        bounds = {bx::min(bounds.min, bx::min(near, far)), bx::max(bounds.max, bx::max(near, far))};
    }

    // The FogVolumes in the grid, nearest the eye first.
    std::vector<const FogVolumeSample*> volumes;
    for (const FogVolumeSample& v : frame.volumes)
        if (v.density > 0.0f && v.bounds.min.x <= bounds.max.x && v.bounds.max.x >= bounds.min.x &&
            v.bounds.min.y <= bounds.max.y && v.bounds.max.y >= bounds.min.y && v.bounds.min.z <= bounds.max.z &&
            v.bounds.max.z >= bounds.min.z)
            volumes.push_back(&v);
    if (!(s.density > 0.0f) && volumes.empty())
        return false; // nothing to scatter: as off
    const auto centerDistance = [&](const FogVolumeSample* v) {
        return bx::length(bx::sub(bx::mul(bx::add(v->bounds.min, v->bounds.max), 0.5f), eye));
    };
    std::ranges::stable_sort(volumes, {}, centerDistance);
    volumes.resize(std::min(volumes.size(), kMaxFogVolumes));
    float volumeMtx[kMaxFogVolumes][16] = {};
    float volumeParams[kMaxFogVolumes * 2][4] = {};
    for (size_t i = 0; i < volumes.size(); ++i) { // packed as cs_volfog_inject.sc's u_fogVolumes says
        const FogVolumeSample& v = *volumes[i];
        std::memcpy(volumeMtx[i], v.worldToLocal.data(), sizeof(volumeMtx[i]));
        const float params[2][4] = {{v.density, v.edgeFalloff, v.heightFalloff, v.shape == FogVolumeShape::Sphere ? 1.0f : 0.0f},
                                    {v.albedo.x, v.albedo.y, v.albedo.z, v.bottom}};
        std::memcpy(volumeParams[i * 2], params, sizeof(params));
    }
    const VoxelGiLights lights =
        packVoxelGiLights(frame.sunDirection, frame.sunColor, frame.sunShadowed, frame.lights, frame.shadows, bounds);

    if (!bgfx::isValid(density_)) {
        density_ = volume();
        scattering_ = {volume(), volume()};
        integrated_ = bgfx::createTexture2D(kFroxelsX * kFroxelSlices, kFroxelsY, false, 1, bgfx::TextureFormat::RGBA16F,
                                            kVolumeFlags);
        history_ = false;
    }
    const bool history = history_ && historyDistance_ == s.maxDistance && s.temporalBlend > 0.0f;
    // Jittered within each froxel (low-discrepancy steps: R2 across, golden ratio along z) only while there is history
    // to average it out.
    const bool jittered = s.temporalBlend > 0.0f;
    const auto step = [&](float a) { return jittered ? std::fmod(0.5f + float(frame_) * a, 1.0f) - 0.5f : 0.0f; };
    const float jitter = step(0.618034f);
    const bx::Vec3 ambient = bx::mul(frame.ambient, s.ambientIntensity);
    // Packed as the cs_volfog_*.sc shaders' u_volFogGrid says.
    const float grid[kGridUniforms][4] = {
        {topLeftStart.x, topLeftStart.y, topLeftStart.z, s.maxDistance},
        {topLeft.x, topLeft.y, topLeft.z, jitter},
        {right.x, right.y, right.z, float(volumes.size())},
        {down.x, down.y, down.z, float(lights.count)},
        {s.density, s.heightFalloff, s.baseHeight, s.anisotropy},
        {s.albedo.x, s.albedo.y, s.albedo.z, float(frame.maps.cascades)},
        {ambient.x, ambient.y, ambient.z, s.lightIntensity},
        {prevEye_.x, prevEye_.y, prevEye_.z, history ? s.temporalBlend : 0.0f},
        {prevForward_.x, prevForward_.y, prevForward_.z, 0.0f},
        {step(0.7548777f), step(0.5698403f), jittered ? 0.0f : 1.0f, 0.0f},
        {startRight.x, startRight.y, startRight.z, 0.0f},
        {startDown.x, startDown.y, startDown.z, 0.0f}};
    const size_t next = 1 - current_;
    bgfx::setViewName(view, "Volumetric fog");
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);

    bgfx::setUniform(grid_, grid, kGridUniforms);
    bgfx::setUniform(volumeMtx_, volumeMtx, kMaxFogVolumes);
    bgfx::setUniform(volumes_, volumeParams, kMaxFogVolumes * 2);
    bgfx::setImage(0, density_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    bgfx::dispatch(view, inject_, kFroxelsX / 4, (kFroxelsY + 3) / 4, kFroxelSlices / 4);

    bgfx::setUniform(grid_, grid, kGridUniforms);
    bgfx::setUniform(prev_, prevViewProj_.data());
    bgfx::setUniform(lights_, lights.data.data(), uint16_t(kMaxVoxelGiLights * 4));
    if (frame.maps.csmMtx)
        bgfx::setUniform(csmMtx_, frame.maps.csmMtx, kMaxShadowCascades + kMaxShadowedLocalLights);
    if (frame.maps.csmBias)
        bgfx::setUniform(csmBias_, frame.maps.csmBias, kMaxShadowCascades);
    bgfx::setImage(0, density_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setTexture(1, historySampler_, scattering_[current_], BGFX_SAMPLER_UVW_CLAMP);
    bgfx::setImage(2, scattering_[next], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    bgfx::setTexture(3, csmSampler_, bgfx::isValid(frame.maps.atlas) ? frame.maps.atlas : noShadow2D_);
    for (size_t cube = 0; cube < kMaxShadowedPointLights; ++cube)
        bgfx::setTexture(uint8_t(4 + cube), cubeSamplers_[cube],
                         bgfx::isValid(frame.maps.cubes[cube]) ? frame.maps.cubes[cube] : noShadowCube_);
    bgfx::dispatch(view, scatter_, kFroxelsX / 4, (kFroxelsY + 3) / 4, kFroxelSlices / 4);

    bgfx::setUniform(grid_, grid, kGridUniforms);
    bgfx::setImage(0, scattering_[next], 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(1, integrated_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    bgfx::dispatch(view, integrate_, kFroxelsX / 8, (kFroxelsY + 7) / 8, 1);

    // The composite over the opaque scene's color by its depth (sampled, so not attached).
    const bgfx::TextureHandle color = bgfx::getTexture(target, 0);
    if (!bgfx::isValid(color_) || color.idx != colorTexture_.idx || width != colorWidth_ || height != colorHeight_) {
        if (bgfx::isValid(color_))
            bgfx::destroy(color_);
        color_ = bgfx::createFrameBuffer(1, &color, false);
        colorTexture_ = color;
        colorWidth_ = width;
        colorHeight_ = height;
    }
    drawParams_ = {{{forward.x, forward.y, forward.z, 1.0f}, {0.0f, s.maxDistance, 0.0f, 0.0f}}};
    const auto composite = static_cast<bgfx::ViewId>(view + 1);
    bgfx::setViewName(composite, "Volumetric fog composite");
    bgfx::setViewFrameBuffer(composite, color_);
    bgfx::setViewRect(composite, 0, 0, width, height);
    bgfx::setViewClear(composite, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(composite, nullptr, frame.projMtx);
    bgfx::setTexture(0, tex0_, bgfx::getTexture(target, 1), BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
    bgfx::setTexture(1, tex1_, integrated_, BGFX_SAMPLER_UVW_CLAMP);
    bgfx::setUniform(drawUniform_, drawParams_.data(), 2);
    bgfx::setVertexBuffer(0, triangle_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_SRC_ALPHA));
    bgfx::submit(composite, composite_);

    std::copy_n(viewProj, 16, prevViewProj_.begin());
    prevEye_ = eye;
    prevForward_ = forward;
    history_ = true;
    historyDistance_ = s.maxDistance;
    current_ = next;
    ++frame_;
    return true;
}

} // namespace kast
