#include "occlusion.h"
#include "cs_hiz_debug.bin.h"
#include "cs_hiz_build.bin.h"
#include "cs_hiz_test.bin.h"
#include "log.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace kast {
namespace {

constexpr float kPendingMark = 2.0f; // in a read-back buffer until the GPU's 0 / 1 values land (never written by it)

float apart(const bx::Vec3& a, const bx::Vec3& b)
{
    return bx::length(bx::sub(a, b));
}

// The largest power of 2 not over half of size (at least 1).
uint16_t halfPow2(uint16_t size)
{
    return static_cast<uint16_t>(std::bit_floor(std::max(1u, size / 2u)));
}

} // namespace

Aabb dilateForOcclusion(const Aabb& box)
{
    const bx::Vec3 extent = bx::sub(box.max, box.min);
    const float pad = std::max(kOcclusionMinDilation, kOcclusionDilation * std::max({extent.x, extent.y, extent.z}));
    return {bx::sub(box.min, pad), bx::add(box.max, pad)};
}

bool contains(const Aabb& outer, const Aabb& inner)
{
    return inner.min.x >= outer.min.x && inner.min.y >= outer.min.y && inner.min.z >= outer.min.z &&
           inner.max.x <= outer.max.x && inner.max.y <= outer.max.y && inner.max.z <= outer.max.z;
}

std::pair<uint16_t, uint16_t> hizSize(uint16_t width, uint16_t height)
{
    return {halfPow2(width), halfPow2(height)};
}

uint8_t hizMipCount(uint16_t width0, uint16_t height0)
{
    return static_cast<uint8_t>(std::bit_width(static_cast<unsigned>(std::max(width0, height0))));
}

std::pair<uint32_t, uint32_t> hizDepthSpan(uint32_t x, uint32_t width, uint32_t width0)
{
    const uint32_t first = x * width / width0;
    const uint32_t last = ((x + 1) * width + width0 - 1) / width0 - 1; // ceil, inclusive
    return {first, std::min(last, width - 1)};
}

std::optional<HizRect> hizProject(const Aabb& box, const float* m, bool homogeneousDepth, bool originBottomLeft)
{
    HizRect rect{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, FLT_MAX};
    for (int corner = 0; corner < 8; ++corner) {
        const float x = corner & 1 ? box.max.x : box.min.x, y = corner & 2 ? box.max.y : box.min.y,
                    z = corner & 4 ? box.max.z : box.min.z;
        const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
        const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        if (cw <= 1e-6f)
            return std::nullopt;
        const float depth = homogeneousDepth ? cz / cw * 0.5f + 0.5f : cz / cw;
        if (depth < 0.0f)
            return std::nullopt;
        const float u = cx / cw * 0.5f + 0.5f;
        const float v = originBottomLeft ? cy / cw * 0.5f + 0.5f : 0.5f - cy / cw * 0.5f;
        rect = {std::min(rect.u0, u), std::min(rect.v0, v), std::max(rect.u1, u), std::max(rect.v1, v),
                std::min(rect.depth, depth)};
    }
    rect.u0 = std::clamp(rect.u0, 0.0f, 1.0f), rect.u1 = std::clamp(rect.u1, 0.0f, 1.0f);
    rect.v0 = std::clamp(rect.v0, 0.0f, 1.0f), rect.v1 = std::clamp(rect.v1, 0.0f, 1.0f);
    return rect;
}

HizFootprint hizFootprint(const HizRect& rect, uint16_t width0, uint16_t height0, uint8_t mips)
{
    const float size = std::max({(rect.u1 - rect.u0) * width0, (rect.v1 - rect.v0) * height0, 1.0f});
    int mip = std::clamp(static_cast<int>(std::ceil(std::log2(size))), 0, mips - 1);
    for (;; ++mip) { // float rounding can leave a span of 3 at the computed mip: one coarser then
        const int w = std::max(1, width0 >> mip), h = std::max(1, height0 >> mip);
        const HizFootprint at{mip, std::clamp(static_cast<int>(rect.u0 * float(w)), 0, w - 1),
                              std::clamp(static_cast<int>(rect.v0 * float(h)), 0, h - 1),
                              std::clamp(static_cast<int>(rect.u1 * float(w)), 0, w - 1),
                              std::clamp(static_cast<int>(rect.v1 * float(h)), 0, h - 1)};
        if ((at.x1 - at.x0 <= 1 && at.y1 - at.y0 <= 1) || mip == mips - 1)
            return at;
    }
}

HizPyramid buildHizPyramid(std::span<const float> depth, uint16_t width, uint16_t height)
{
    HizPyramid pyramid;
    std::tie(pyramid.width0, pyramid.height0) = hizSize(width, height);
    const uint8_t mips = hizMipCount(pyramid.width0, pyramid.height0);
    pyramid.mips.resize(mips);
    std::vector<float>& base = pyramid.mips[0];
    base.resize(size_t(pyramid.width0) * pyramid.height0);
    for (uint32_t y = 0; y < pyramid.height0; ++y)
        for (uint32_t x = 0; x < pyramid.width0; ++x) {
            const auto [x0, x1] = hizDepthSpan(x, width, pyramid.width0);
            const auto [y0, y1] = hizDepthSpan(y, height, pyramid.height0);
            float d = 0.0f;
            for (uint32_t sy = y0; sy <= y1; ++sy)
                for (uint32_t sx = x0; sx <= x1; ++sx)
                    d = std::max(d, depth[size_t(sy) * width + sx]);
            base[size_t(y) * pyramid.width0 + x] = d;
        }
    for (uint8_t mip = 1; mip < mips; ++mip) {
        const int sw = std::max(1, pyramid.width0 >> (mip - 1)), sh = std::max(1, pyramid.height0 >> (mip - 1));
        const int w = std::max(1, sw >> 1), h = std::max(1, sh >> 1);
        pyramid.mips[mip].resize(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float d = 0.0f;
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx)
                        d = std::max(d, pyramid.mips[mip - 1][size_t(std::min(2 * y + sy, sh - 1)) * sw +
                                                             size_t(std::min(2 * x + sx, sw - 1))]);
                pyramid.mips[mip][size_t(y) * w + x] = d;
            }
    }
    return pyramid;
}

bool hizOccluded(const HizPyramid& pyramid, const Aabb& box, const float* viewProj, bool homogeneousDepth,
                 bool originBottomLeft)
{
    const std::optional<HizRect> rect = hizProject(box, viewProj, homogeneousDepth, originBottomLeft);
    if (!rect)
        return false;
    const HizFootprint at =
        hizFootprint(*rect, pyramid.width0, pyramid.height0, static_cast<uint8_t>(pyramid.mips.size()));
    const int w = std::max(1, pyramid.width0 >> at.mip);
    float farthest = 0.0f;
    for (int y = at.y0; y <= at.y1; ++y)
        for (int x = at.x0; x <= at.x1; ++x)
            farthest = std::max(farthest, pyramid.mips[size_t(at.mip)][size_t(y) * w + x]);
    return rect->depth > farthest;
}

// --- GPU ---

void HizOcclusion::init()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    const uint64_t needed = BGFX_CAPS_COMPUTE | BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
    if (forceUnsupported_ || (caps->supported & needed) != needed ||
        !(caps->formats[bgfx::TextureFormat::R32F] & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE)) {
        log::info(std::string("Occlusion culling: ") +
                  (forceUnsupported_ ? "off (--occlusion-unsupported)" : "this GPU lacks compute, blit or read-back") +
                  "; every draw in the frustum is drawn");
        return;
    }
    supported_ = true;
    const auto program = [](const uint8_t* code, size_t size) {
        return bgfx::createProgram(bgfx::createShader(bgfx::makeRef(code, uint32_t(size))), true);
    };
    buildProgram_ = program(cs_hiz_build, sizeof(cs_hiz_build));
    for (bgfx::TextureHandle& spare : spares_)
        spare = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_COMPUTE_WRITE);
    testProgram_ = program(cs_hiz_test, sizeof(cs_hiz_test));
    if (caps->formats[bgfx::TextureFormat::RGBA8] & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE)
        debugProgram_ = program(cs_hiz_debug, sizeof(cs_hiz_debug));
    params_ = bgfx::createUniform("u_hiz", bgfx::UniformType::Vec4, 2);
    viewProj_ = bgfx::createUniform("u_hizViewProj", bgfx::UniformType::Mat4);
    depthSampler_ = bgfx::createUniform("s_hizDepth", bgfx::UniformType::Sampler);
    pyramidSampler_ = bgfx::createUniform("s_hizPyramid", bgfx::UniformType::Sampler);
    boxLayout_.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
    boxes_ = bgfx::createDynamicVertexBuffer(2, boxLayout_, BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_ALLOW_RESIZE);
}

void HizOcclusion::shutdown()
{
    if (!supported_)
        return;
    destroyTargets();
    destroyVisibility();
    for (bgfx::TextureHandle spare : spares_)
        bgfx::destroy(spare);
    for (bgfx::ProgramHandle program : {buildProgram_, testProgram_, debugProgram_})
        if (bgfx::isValid(program))
            bgfx::destroy(program);
    for (bgfx::UniformHandle uniform : {params_, viewProj_, depthSampler_, pyramidSampler_})
        bgfx::destroy(uniform);
    bgfx::destroy(boxes_);
    supported_ = false;
}

void HizOcclusion::destroyTargets()
{
    for (bgfx::TextureHandle* texture : {&depthCopy_, &pyramid_, &debug_}) {
        if (bgfx::isValid(*texture))
            bgfx::destroy(*texture);
        *texture = BGFX_INVALID_HANDLE;
    }
    debugBuilt_ = -1;
}

void HizOcclusion::destroyVisibility()
{
    if (bgfx::isValid(visibility_))
        bgfx::destroy(visibility_);
    visibility_ = BGFX_INVALID_HANDLE;
    for (Pending& pending : pending_) {
        if (bgfx::isValid(pending.readback))
            bgfx::destroy(pending.readback);
        pending.readback = BGFX_INVALID_HANDLE;
    }
    rows_ = 0;
}

void HizOcclusion::reset()
{
    hidden_.clear();
    staged_.clear();
    nextStage_ = 0;
    useful_ = true;
    for (Pending& pending : pending_)
        pending.candidates.clear(); // still waiting (its read-back lands in data): resolves to nothing
}

void HizOcclusion::beginFrame(const bx::Vec3& eye, uint16_t width, uint16_t height)
{
    ++frame_;
    if (!supported_)
        return;
    if (width != width_ || height != height_) {
        destroyTargets();
        reset();
        width_ = width, height_ = height;
        if (width > 0 && height > 0) {
            std::tie(width0_, height0_) = hizSize(width, height);
            mips_ = hizMipCount(width0_, height0_);
            depthCopy_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8,
                                               BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
            pyramid_ = bgfx::createTexture2D(width0_, height0_, true, 1, bgfx::TextureFormat::R32F,
                                             BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
        }
    }
    if (apart(eye, resultEye_) > kOcclusionMaxEyeMove)
        hidden_.clear();
    // Read back a frame after the blit: Direct3D 11 maps the read-back texture at once, waiting for the GPU to finish
    // the blit, so asking in the blit's own frame would stall every frame.
    for (Pending& pending : pending_)
        if (pending.waiting && !pending.requested && frame_ >= pending.blitFrame + 3) {
            std::ranges::fill(pending.data, kPendingMark);
            bgfx::readTexture(pending.readback, pending.data.data());
            pending.requested = true;
        }
    // Landed read-backs, oldest test first, so the newest wins.
    std::array<Pending*, 6> landed{};
    size_t count = 0;
    for (Pending& pending : pending_)
        if (pending.requested && pending.data.front() != kPendingMark && pending.data.back() != kPendingMark)
            landed[count++] = &pending;
    std::sort(landed.begin(), landed.begin() + count,
              [](const Pending* a, const Pending* b) { return a->depthFrame < b->depthFrame; });
    for (size_t i = 0; i < count; ++i) {
        Pending& pending = *landed[i];
        pending.waiting = pending.requested = false;
        if (pending.candidates.empty() || apart(pending.eye, eye) > kOcclusionMaxEyeMove)
            continue;
        hidden_.clear();
        for (size_t c = 0; c < pending.candidates.size(); ++c)
            if (pending.data[c] < 0.5f)
                hidden_.insert_or_assign(pending.candidates[c].id, pending.candidates[c].box);
        resultEye_ = pending.eye;
        useful_ = float(hidden_.size()) >= kOcclusionUsefulFraction * float(pending.candidates.size());
        latency_ = static_cast<int>(frame_ - pending.depthFrame);
    }
}

bool HizOcclusion::occluded(uint64_t id, const Aabb& bounds) const
{
    const auto it = hidden_.find(id);
    return it != hidden_.end() && contains(it->second, bounds);
}

void HizOcclusion::endFrame(bgfx::ViewId view, bgfx::ViewId copyView, bgfx::TextureHandle depth,
                            const float* viewProj, const bx::Vec3& eye, std::vector<Candidate> candidates)
{
    if (!supported_ || !bgfx::isValid(depthCopy_))
        return;
    const auto idle = std::ranges::find_if(pending_, [](const Pending& pending) { return !pending.waiting; });
    const auto rows = uint32_t(staged_.size() + kOcclusionImageWidth - 1) / kOcclusionImageWidth;
    if (rows > rows_ && std::ranges::none_of(pending_, [](const Pending& p) { return p.waiting; })) {
        // Grown only while no read-back is in flight: one may still be writing into its data.
        destroyVisibility();
        rows_ = std::bit_ceil(rows);
        visibility_ = bgfx::createTexture2D(uint16_t(kOcclusionImageWidth), uint16_t(rows_), false, 1,
                                            bgfx::TextureFormat::R32F, BGFX_TEXTURE_COMPUTE_WRITE);
        for (Pending& pending : pending_) {
            pending.readback = bgfx::createTexture2D(uint16_t(kOcclusionImageWidth), uint16_t(rows_), false, 1,
                                                     bgfx::TextureFormat::R32F,
                                                     BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            pending.data.assign(size_t(kOcclusionImageWidth) * rows_, kPendingMark);
        }
    }
    if (!staged_.empty() && idle != pending_.end() && rows <= rows_) {
        // The pyramid from last frame's depth, 4 mips a pass (cs_hiz_build.sc): mip 0 the farthest depth under each
        // texel, then 2 x 2 maxima. Fewer, bigger dispatches: each one costs a few microseconds whatever its size.
        // ponytail: 3 passes at 720p; one pass with a global atomic counter (AMD SPD) if those few still show.
        for (uint8_t first = 0; first < mips_; first = uint8_t(first + 4)) {
            const int w = std::max(1, width0_ >> first), h = std::max(1, height0_ >> first);
            const int sw = first == 0 ? width_ : std::max(1, width0_ >> (first - 1));
            const int sh = first == 0 ? height_ : std::max(1, height0_ >> (first - 1));
            const int levels = std::min(4, mips_ - first);
            const float params[2][4] = {{float(sw), float(sh), float(w), float(h)},
                                        {first == 0 ? 1.0f : 0.0f, float(levels), 0.0f, 0.0f}};
            bgfx::setTexture(0, depthSampler_, depthCopy_);
            if (first == 0)
                bgfx::setImage(1, spares_[3], 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::R32F);
            else
                bgfx::setImage(1, pyramid_, uint8_t(first - 1), bgfx::Access::ReadWrite, bgfx::TextureFormat::R32F);
            for (int level = 0; level < 4; ++level) // past the last mip: a spare (written by none; one UAV per stage)
                if (level < levels)
                    bgfx::setImage(uint8_t(2 + level), pyramid_, uint8_t(first + level), bgfx::Access::Write,
                                   bgfx::TextureFormat::R32F);
                else
                    bgfx::setImage(uint8_t(2 + level), spares_[size_t(level - 1)], 0, bgfx::Access::Write,
                                   bgfx::TextureFormat::R32F);
            bgfx::setUniform(params_, params, 2);
            bgfx::dispatch(view, buildProgram_, (w + 7u) / 8u, (h + 7u) / 8u);
        }
        // Last frame's boxes against it, through last frame's viewProj; every texel written (1 past the boxes).
        const bgfx::Memory* memory = bgfx::alloc(uint32_t(staged_.size() * 2 * 4 * sizeof(float)));
        auto* out = reinterpret_cast<float*>(memory->data);
        for (const Candidate& candidate : staged_) {
            const float box[8] = {candidate.box.min.x, candidate.box.min.y, candidate.box.min.z, 0.0f,
                                  candidate.box.max.x, candidate.box.max.y, candidate.box.max.z, 0.0f};
            out = std::copy_n(box, 8, out);
        }
        bgfx::update(boxes_, 0, memory);
        const bgfx::Caps* caps = bgfx::getCaps();
        const float test[2][4] = {{float(width0_), float(height0_), float(mips_), float(staged_.size())},
                                  {caps->homogeneousDepth ? 1.0f : 0.0f, caps->originBottomLeft ? 1.0f : 0.0f,
                                   float(kOcclusionImageWidth), 0.0f}};
        bgfx::setTexture(0, pyramidSampler_, pyramid_);
        bgfx::setBuffer(1, boxes_, bgfx::Access::Read);
        bgfx::setImage(2, visibility_, 0, bgfx::Access::Write, bgfx::TextureFormat::R32F);
        bgfx::setUniform(viewProj_, stagedViewProj_);
        bgfx::setUniform(params_, test, 2);
        bgfx::dispatch(view, testProgram_, kOcclusionImageWidth * rows_ / 64u);
        Pending& pending = *idle;
        bgfx::blit(copyView, pending.readback, 0, 0, visibility_);
        pending.blitFrame = frame_;
        pending.candidates = std::move(staged_);
        pending.eye = stagedEye_;
        pending.depthFrame = stagedFrame_;
        pending.waiting = true;
        if (debugMip_ >= 0 && bgfx::isValid(debugProgram_)) {
            const int mip = std::min(debugMip_, mips_ - 1);
            const auto w = uint16_t(std::max(1, width0_ >> mip)), h = uint16_t(std::max(1, height0_ >> mip));
            if (debugBuilt_ != mip) {
                if (bgfx::isValid(debug_))
                    bgfx::destroy(debug_);
                debug_ = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA8,
                                               BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
                debugBuilt_ = mip;
            }
            const float picture[2][4] = {{float(mip), float(w), float(h), 0.0f}, {}};
            bgfx::setTexture(0, pyramidSampler_, pyramid_);
            bgfx::setImage(1, debug_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA8);
            bgfx::setUniform(params_, picture, 2);
            bgfx::dispatch(view, debugProgram_, (w + 7u) / 8u, (h + 7u) / 8u);
        }
    }
    // This frame's, for the next: its depth as view drew it (copyView's blits run before copyView's draws).
    staged_ = std::move(candidates);
    if (staged_.size() < kOcclusionMinDraws || frame_ < nextStage_)
        staged_.clear();
    else
        nextStage_ = frame_ + (useful_ ? 1 : kOcclusionIdleInterval);
    if (staged_.size() > kOcclusionMaxDraws)
        staged_.erase(staged_.begin() + kOcclusionMaxDraws, staged_.end());
    for (Candidate& candidate : staged_)
        candidate.box = dilateForOcclusion(candidate.box);
    std::copy_n(viewProj, 16, stagedViewProj_);
    stagedEye_ = eye;
    stagedFrame_ = frame_;
    if (!staged_.empty())
        bgfx::blit(copyView, depthCopy_, 0, 0, depth);
}

} // namespace kast
