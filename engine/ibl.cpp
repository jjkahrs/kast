#include "ibl.h"

#include "files.h"
#include "renderer.h"
#include "sky.h"

#include "cs_ibl.bin.h"

#include <bimg/bimg.h>
#include <bx/allocator.h>
#include <bx/error.h>
#include <bx/readerwriter.h>
#include <bx/uint32_t.h>
#include <bx/math.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace kast {
namespace {

constexpr uint16_t kShSize = 16; // texels per side of the procedural sky's cube projectSh9 reads
constexpr uint16_t kMaxShSize = 64; // cubeFileSh9 reads the first mip at most this wide
// Y_lm's constants for the basis 1, y, z, x, xy, yz, 3z^2 - 1, xz, x^2 - y^2 (Sloan, "Stupid SH Tricks").
constexpr float kShBasis[9] = {0.282095f, 0.488603f, 0.488603f, 0.488603f, 1.092548f,
                               1.092548f, 0.315392f, 1.092548f, 0.546274f};
// The clamped cosine's convolution per band over pi: pi, 2 pi / 3, pi / 4 (Ramamoorthi & Hanrahan 2001), each / pi.
constexpr float kShCosine[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
constexpr int kProbeShVersion = 1;

std::array<float, 9> shBasis(const bx::Vec3& n)
{
    return {1.0f, n.y, n.z, n.x, n.x * n.y, n.y * n.z, 3.0f * n.z * n.z - 1.0f, n.x * n.z, n.x * n.x - n.y * n.y};
}

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

class StringWriter : public bx::WriterI {
public:
    explicit StringWriter(std::string& out) : out_(out) {}
    int32_t write(const void* data, int32_t size, bx::Error*) override
    {
        out_.append(static_cast<const char*>(data), static_cast<size_t>(size));
        return size;
    }

private:
    std::string& out_;
};

} // namespace

uint8_t envMips(uint16_t size)
{
    uint8_t mips = 1;
    for (uint16_t s = size; s > kEnvSmallestMip; s /= 2)
        ++mips;
    return mips;
}

bx::Vec3 cubeTexelDirection(int side, float u, float v)
{
    // D3D's cube layout (as cs_ibl.sc cubeDirection), then the cube's -z is Kast's +z.
    bx::Vec3 d{0.0f, 0.0f, 0.0f};
    switch (side) {
    case 0: d = {1.0f, -v, -u}; break;
    case 1: d = {-1.0f, -v, u}; break;
    case 2: d = {u, 1.0f, v}; break;
    case 3: d = {u, -1.0f, -v}; break;
    case 4: d = {u, -v, 1.0f}; break;
    default: d = {-u, -v, -1.0f}; break;
    }
    return {d.x, d.y, -d.z};
}

Sh9 projectSh9(const std::array<std::vector<float>, 6>& sides, uint16_t size)
{
    std::array<double, 27> sum{};
    double weights = 0.0;
    for (int side = 0; side < 6; ++side)
        for (uint16_t y = 0; y < size; ++y)
            for (uint16_t x = 0; x < size; ++x) {
                const bx::Vec3 d = cubeTexelDirection(side, (x + 0.5f) / size * 2.0f - 1.0f, (y + 0.5f) / size * 2.0f - 1.0f);
                const float length2 = bx::dot(d, d);
                const double weight = 1.0 / (length2 * std::sqrt(length2)); // the texel's solid angle, up to a constant
                const std::array<float, 9> basis = shBasis(bx::mul(d, 1.0f / std::sqrt(length2)));
                const float* rgb = &sides[static_cast<size_t>(side)][(static_cast<size_t>(y) * size + x) * 3];
                for (size_t i = 0; i < 9; ++i)
                    for (size_t c = 0; c < 3; ++c)
                        sum[i * 3 + c] += rgb[c] * basis[i] * weight;
                weights += weight;
            }
    const double solidAngle = 4.0 * bx::kPi / weights; // the weights over the sphere sum to 4 pi
    Sh9 sh;
    for (size_t i = 0; i < 27; ++i)
        sh[i] = static_cast<float>(sum[i] * kShBasis[i / 3] * kShBasis[i / 3] * kShCosine[i / 3] * solidAngle);
    return sh;
}

bx::Vec3 shIrradiance(const Sh9& sh, const bx::Vec3& normal)
{
    const std::array<float, 9> basis = shBasis(normal);
    float sum[3] = {};
    for (size_t i = 0; i < 9; ++i)
        for (size_t c = 0; c < 3; ++c)
            sum[c] += sh[i * 3 + c] * basis[i];
    return {std::max(sum[0], 0.0f), std::max(sum[1], 0.0f), std::max(sum[2], 0.0f)};
}

Sh9 scaleSh9(const Sh9& sh, const bx::Vec3& tint)
{
    Sh9 scaled;
    const float rgb[3] = {tint.x, tint.y, tint.z};
    for (size_t i = 0; i < 27; ++i)
        scaled[i] = sh[i] * rgb[i % 3];
    return scaled;
}

Sh9 proceduralSkySh9(const SceneSky& sky)
{
    std::array<std::vector<float>, 6> sides;
    for (int side = 0; side < 6; ++side)
        for (uint16_t y = 0; y < kShSize; ++y)
            for (uint16_t x = 0; x < kShSize; ++x) {
                const bx::Vec3 d = bx::normalize(
                    cubeTexelDirection(side, (x + 0.5f) / kShSize * 2.0f - 1.0f, (y + 0.5f) / kShSize * 2.0f - 1.0f));
                const bx::Vec3 color = proceduralSkyColor(sky, d.y);
                sides[static_cast<size_t>(side)].insert(sides[static_cast<size_t>(side)].end(), {color.x, color.y, color.z});
            }
    return projectSh9(sides, kShSize);
}

std::optional<Sh9> cubeFileSh9(std::string_view bytes, bool srgb)
{
    bimg::ImageContainer image{};
    bx::Error err;
    if (bytes.size() > UINT32_MAX || !bimg::imageParse(image, bytes.data(), static_cast<uint32_t>(bytes.size()), &err) ||
        !image.m_cubeMap || image.m_format >= bimg::TextureFormat::Count)
        return std::nullopt;
    uint8_t lod = 0;
    while (lod + 1 < image.m_numMips && (image.m_width >> lod) > kMaxShSize)
        ++lod;
    bx::DefaultAllocator allocator;
    std::array<std::vector<float>, 6> sides;
    uint16_t size = 0;
    for (uint16_t side = 0; side < 6; ++side) {
        bimg::ImageMip mip;
        if (!bimg::imageGetRawData(image, side, lod, bytes.data(), static_cast<uint32_t>(bytes.size()), mip))
            return std::nullopt;
        size = static_cast<uint16_t>(mip.m_width);
        std::vector<float> rgba(static_cast<size_t>(mip.m_width) * mip.m_height * 4);
        bimg::imageDecodeToRgba32f(&allocator, rgba.data(), mip.m_data, mip.m_width, mip.m_height, 1, mip.m_width * 16,
                                   mip.m_format);
        std::vector<float>& rgb = sides[side];
        for (size_t i = 0; i < rgba.size(); i += 4)
            for (size_t c = 0; c < 3; ++c)
                rgb.push_back(srgb ? srgbToLinear(rgba[i + c]) : rgba[i + c]);
    }
    return projectSh9(sides, size);
}

std::array<float, 2> envBrdf(float roughness, float nv)
{
    const float r[4] = {roughness * -1.0f + 1.0f, roughness * -0.0275f + 0.0425f, roughness * -0.572f + 1.04f,
                        roughness * 0.022f - 0.04f};
    const float a004 = std::min(r[0] * r[0], std::exp2(-9.28f * nv)) * r[0] + r[1];
    // Clamped: the fit strays a little past 0..1 at its extremes (-0.04 at roughness 0, grazing).
    return {std::clamp(-1.04f * a004 + r[2], 0.0f, 1.0f), std::clamp(1.04f * a004 + r[3], 0.0f, 1.0f)};
}

bx::Vec3 boxProjectedDirection(const bx::Vec3& r, const bx::Vec3& p, const bx::Vec3& center, const bx::Vec3& halfSize)
{
    const bx::Vec3 lo = bx::sub(center, halfSize), hi = bx::add(center, halfSize);
    if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z)
        return r;
    // As fs_mesh.sc: the distance along r to each axis' exit plane, the nearest of them.
    const auto exit = [](float rc, float pc, float loc, float hic) {
        return std::abs((rc >= 0.0f ? hic : loc) - pc) / std::max(std::abs(rc), 1e-5f);
    };
    const float t = std::min({exit(r.x, p.x, lo.x, hi.x), exit(r.y, p.y, lo.y, hi.y), exit(r.z, p.z, lo.z, hi.z)});
    return bx::sub(bx::add(p, bx::mul(r, t)), center);
}

std::optional<size_t> selectProbe(const std::vector<ProbeBox>& probes, const bx::Vec3& point)
{
    std::optional<size_t> best;
    float bestDistance = FLT_MAX;
    for (size_t i = 0; i < probes.size(); ++i) {
        const ProbeBox& probe = probes[i];
        const bx::Vec3 offset = bx::sub(point, probe.center);
        if (std::abs(offset.x) > probe.halfSize.x || std::abs(offset.y) > probe.halfSize.y ||
            std::abs(offset.z) > probe.halfSize.z)
            continue;
        const float distance = bx::dot(offset, offset);
        if (!best || probe.priority > probes[*best].priority ||
            (probe.priority == probes[*best].priority && distance < bestDistance)) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

BakedProbe makeBakedProbe(uint16_t size, const std::vector<std::vector<uint16_t>>& levels)
{
    std::string pixels; // a KTX holds each level's six sides in turn: as read back
    for (const std::vector<uint16_t>& level : levels)
        pixels.append(reinterpret_cast<const char*>(level.data()), level.size() * sizeof(uint16_t));
    BakedProbe probe{.size = size};
    StringWriter writer(probe.ktx);
    bx::Error err;
    bimg::imageWriteKtx(&writer, bimg::TextureFormat::RGBA16F, true, size, size, 1, static_cast<uint8_t>(levels.size()), 1,
                        false, pixels.data(), &err);
    std::array<std::vector<float>, 6> sides;
    const size_t texels = static_cast<size_t>(size) * size;
    for (size_t side = 0; side < 6; ++side)
        for (size_t i = 0; i < texels; ++i)
            for (size_t c = 0; c < 3; ++c)
                sides[side].push_back(bx::halfToFloat(levels[0][(side * texels + i) * 4 + c]));
    probe.sh = projectSh9(sides, size);
    return probe;
}

std::string bakedProbePath(const std::string& scenePath, EntityId id)
{
    const std::filesystem::path scene(reinterpret_cast<const char8_t*>(scenePath.c_str()));
    const std::filesystem::path file =
        scene.parent_path() / (scene.stem().u8string() + u8"_probes") / std::format("{}.ktx", id);
    const std::u8string text = file.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path probeShPath(const std::filesystem::path& ktx)
{
    std::filesystem::path json = ktx;
    return json.replace_extension(".json");
}

std::string probeShJson(uint16_t size, const Sh9& sh)
{
    nlohmann::ordered_json json;
    json["version"] = kProbeShVersion;
    json["size"] = size;
    json["sh"] = nlohmann::ordered_json::array();
    for (size_t i = 0; i < 27; i += 3)
        json["sh"].push_back({sh[i], sh[i + 1], sh[i + 2]});
    return json.dump(2) + "\n";
}

bool parseProbeShJson(std::string_view text, uint16_t& size, Sh9& sh, std::string& error)
{
    const nlohmann::ordered_json json = nlohmann::ordered_json::parse(text, nullptr, false);
    const auto number = [](const nlohmann::ordered_json& value) { return value.is_number() && std::isfinite(value.get<double>()); };
    if (!json.is_object() || json.value("version", 0) != kProbeShVersion || !json.contains("size") ||
        !json["size"].is_number_unsigned() || !json.contains("sh") || !json["sh"].is_array() || json["sh"].size() != 9) {
        error = "its .json is not a version 1 probe sidecar";
        return false;
    }
    for (size_t i = 0; i < 9; ++i) {
        const nlohmann::ordered_json& c = json["sh"][i];
        if (!c.is_array() || c.size() != 3 || !number(c[0]) || !number(c[1]) || !number(c[2])) {
            error = "its .json has a malformed \"sh\"";
            return false;
        }
        for (size_t channel = 0; channel < 3; ++channel)
            sh[i * 3 + channel] = c[channel].get<float>();
    }
    size = json["size"].get<uint16_t>();
    return true;
}

bool writeBakedProbe(const std::filesystem::path& ktx, const BakedProbe& probe, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(ktx.parent_path(), ec);
    if (!writeFileAtomic(ktx, probe.ktx) || !writeFileAtomic(probeShPath(ktx), probeShJson(probe.size, probe.sh))) {
        error = std::format("cannot write '{}'", ktx.string());
        return false;
    }
    return true;
}

bool checkBakedProbe(std::string_view ktx, std::string_view json, uint16_t resolution, Sh9& sh, std::string& error)
{
    const std::optional<GpuImageInfo> info = parseGpuImage(ktx, true, error);
    if (!info)
        return false;
    uint16_t size = 0;
    if (!parseProbeShJson(json, size, sh, error))
        return false;
    uint8_t fullMips = 1;
    for (uint16_t s = resolution; s > 1; s /= 2)
        ++fullMips;
    if (info->width != resolution || size != resolution) {
        error = std::format("baked at {}, but its resolution is {} (bake it again)", info->width, resolution);
        return false;
    }
    if (info->format != bgfx::TextureFormat::RGBA16F || info->mips != fullMips) {
        error = "not an RGBA16F cube with a full mip chain (bake it again)";
        return false;
    }
    return true;
}

void EnvironmentFilter::init()
{
    if (!(bgfx::getCaps()->supported & BGFX_CAPS_COMPUTE))
        return;
    program_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_ibl, sizeof(cs_ibl))), true);
    params_ = bgfx::createUniform("u_ibl", bgfx::UniformType::Vec4, 4);
    source_ = bgfx::createUniform("s_iblSource", bgfx::UniformType::Sampler);
}

void EnvironmentFilter::shutdown()
{
    if (!supported())
        return;
    bgfx::destroy(program_);
    bgfx::destroy(params_);
    bgfx::destroy(source_);
    program_ = BGFX_INVALID_HANDLE;
}

void EnvironmentFilter::dispatch(bgfx::ViewId view, bgfx::TextureHandle source, bgfx::TextureHandle target, uint8_t mip,
                                 uint16_t size, const float (&params)[4][4])
{
    bgfx::setUniform(params_, params, 4);
    bgfx::setTexture(0, source_, source);
    bgfx::setImage(1, target, mip, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    const uint32_t groups = (size + 7u) / 8u;
    bgfx::dispatch(view, program_, groups, groups, 6);
}

bgfx::TextureHandle EnvironmentFilter::sky(bgfx::ViewId view, const SceneSky& sky, bgfx::TextureHandle skyCube,
                                           uint16_t skyCubeSize)
{
    if (!supported())
        return BGFX_INVALID_HANDLE;
    const bool cube = bgfx::isValid(skyCube);
    const bgfx::TextureHandle source =
        bgfx::createTextureCube(kSkyEnvSize, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_COMPUTE_WRITE);
    // The gradient reads no texture; something is bound all the same (the D3D11 debug layer wants a cube there).
    const bgfx::TextureHandle input = cube ? skyCube
                                           : bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA16F,
                                                                     BGFX_TEXTURE_COMPUTE_WRITE);
    const float lod = cube ? std::max(std::log2(static_cast<float>(skyCubeSize) / kSkyEnvSize), 0.0f) : 0.0f;
    const float params[4][4] = {{0.0f, float(kSkyEnvSize), 0.0f, float(kSkyEnvSize)},
                                {sky.top.x, sky.top.y, sky.top.z, cube ? 1.0f : 0.0f},
                                {sky.horizon.x, sky.horizon.y, sky.horizon.z, lod},
                                {sky.bottom.x, sky.bottom.y, sky.bottom.z, 0.0f}};
    dispatch(view, input, source, 0, kSkyEnvSize, params);
    const bgfx::TextureHandle env = prefilter(view, source, kSkyEnvSize);
    bgfx::destroy(source); // after this frame's passes: bgfx destroys once the frame is rendered
    if (!cube)
        bgfx::destroy(input);
    return env;
}

bgfx::TextureHandle EnvironmentFilter::prefilter(bgfx::ViewId view, bgfx::TextureHandle source, uint16_t size,
                                                 uint64_t flags)
{
    if (!supported())
        return BGFX_INVALID_HANDLE;
    uint8_t levels = 1; // the full chain, down to 1 x 1
    for (uint16_t s = size; s > 1; s /= 2)
        ++levels;
    // The source's box-filtered mips, for the prefilter's lod lookups.
    const bgfx::TextureHandle chain =
        bgfx::createTextureCube(size, true, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_COMPUTE_WRITE);
    for (uint8_t level = 0; level < levels; ++level) {
        const auto levelSize = static_cast<uint16_t>(size >> level);
        const float params[4][4] = {{1.0f, float(levelSize), float(1 << level), float(size)}, {}, {}, {}};
        dispatch(view, source, chain, level, levelSize, params);
    }
    // Every level of the result is filled: those past envMips(size) - 1 are roughness 1 too, smaller.
    const bgfx::TextureHandle env = bgfx::createTextureCube(
        size, true, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_UVW_CLAMP | flags);
    const float roughest = static_cast<float>(envMips(size) - 1);
    for (uint8_t level = 0; level < levels; ++level) {
        const auto levelSize = static_cast<uint16_t>(size >> level);
        const float params[4][4] = {{2.0f, float(levelSize), std::min(level / roughest, 1.0f), float(size)},
                                    {},
                                    {0.0f, 0.0f, 0.0f, float(levels)},
                                    {0.0f, 0.0f, 0.0f, float(kEnvSamples)}};
        dispatch(view, chain, env, level, levelSize, params);
    }
    bgfx::destroy(chain);
    return env;
}

} // namespace kast
