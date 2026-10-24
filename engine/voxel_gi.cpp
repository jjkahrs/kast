#include "voxel_gi.h"

#include "cs_voxelgi_bounce.bin.h"
#include "cs_voxelgi_inject.bin.h"
#include "cs_voxelgi_mip.bin.h"

#include "files.h"
#include "lightmap.h"
#include "log.h"
#include "terrain.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <numeric>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr char kVoxelsMagic[4] = {'K', 'V', 'O', 'X'};

uint8_t unorm8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
float fromUnorm8(uint32_t v) { return float(v & 0xffu) / 255.0f; }

uint32_t packRgba8(float r, float g, float b, float a)
{
    return uint32_t(unorm8(r)) | uint32_t(unorm8(g)) << 8 | uint32_t(unorm8(b)) << 16 | uint32_t(unorm8(a)) << 24;
}

// Ward's RGBE: three 8-bit mantissas sharing an 8-bit exponent (128 + e).
uint32_t packRgbe(const bx::Vec3& c)
{
    const float m = std::max({c.x, c.y, c.z});
    if (!(m > 1e-30f))
        return 0;
    int e = 0;
    const float scale = std::frexp(m, &e) * 256.0f / m;
    const auto mantissa = [&](float v) { return uint32_t(std::clamp(int(std::max(v, 0.0f) * scale), 0, 255)); };
    return mantissa(c.x) | mantissa(c.y) << 8 | mantissa(c.z) << 16 | uint32_t(std::clamp(e + 128, 0, 255)) << 24;
}

bx::Vec3 unpackRgbe(uint32_t v)
{
    const uint32_t e = v >> 24;
    if (e == 0)
        return {0.0f, 0.0f, 0.0f};
    const float f = std::ldexp(1.0f, int(e) - 136);
    return {float(v & 0xffu) * f, float((v >> 8) & 0xffu) * f, float((v >> 16) & 0xffu) * f};
}

template <typename T>
void put(std::string& out, const T& value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool get(std::string_view bytes, size_t& at, T& value)
{
    if (bytes.size() - at < sizeof(T))
        return false;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

std::string hex(uint64_t value) { return std::format("{:016x}", value); }

bool parseHex(const Json& value, uint64_t& out)
{
    if (!value.is_string() || value.get<std::string>().size() != 16)
        return false;
    out = 0;
    for (char c : value.get<std::string>()) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0)
            return false;
        out = out * 16 + uint64_t(digit);
    }
    return true;
}

bool finite(const Json& value) { return value.is_number() && std::isfinite(value.get<double>()); }

bx::Vec3 transformPoint(const bx::Vec3& p, const Mat4& m) { return bx::mul(p, m.data()); }

// Sums of what a voxel's samples saw.
struct VoxelSum {
    bx::Vec3 albedo{0.0f, 0.0f, 0.0f}, normal{0.0f, 0.0f, 0.0f}, emission{0.0f, 0.0f, 0.0f};
    uint32_t samples = 0;
};

// Samples triangle (a, b, c) with normals na, nb, nc at most half a voxel apart into sums (slot: the grid's voxel to
// its sums, -1 none yet).
void voxelizeTriangle(const VoxelGrid& grid, const std::array<bx::Vec3, 3>& p, const std::array<bx::Vec3, 3>& n,
                      const BakeSurface& surface, std::vector<int32_t>& slot, std::vector<VoxelSum>& sums)
{
    const bx::Vec3 lo = bx::min(bx::min(p[0], p[1]), p[2]), hi = bx::max(bx::max(p[0], p[1]), p[2]);
    const bx::Vec3 gridLo = grid.origin, gridHi = bx::add(grid.origin, grid.extent());
    if (hi.x < gridLo.x || hi.y < gridLo.y || hi.z < gridLo.z || lo.x > gridHi.x || lo.y > gridHi.y || lo.z > gridHi.z)
        return;
    const float longest = std::max({bx::length(bx::sub(p[1], p[0])), bx::length(bx::sub(p[2], p[1])),
                                    bx::length(bx::sub(p[0], p[2]))});
    // ponytail: the whole triangle is sampled even where it leaves the box; clip it first if huge planes get slow.
    const int steps = std::clamp(int(std::ceil(longest / (0.5f * grid.voxelSize))), 1, 4096);
    for (int i = 0; i <= steps; ++i)
        for (int j = 0; i + j <= steps; ++j) {
            const float u = float(i) / float(steps), v = float(j) / float(steps), w = 1.0f - u - v;
            const bx::Vec3 point = bx::add(bx::add(bx::mul(p[0], w), bx::mul(p[1], u)), bx::mul(p[2], v));
            const std::optional<std::array<int, 3>> cell = grid.voxelAt(point);
            if (!cell)
                continue;
            const size_t index = grid.index((*cell)[0], (*cell)[1], (*cell)[2]);
            if (slot[index] < 0) {
                slot[index] = static_cast<int32_t>(sums.size());
                sums.emplace_back();
            }
            VoxelSum& sum = sums[static_cast<size_t>(slot[index])];
            sum.albedo = bx::add(sum.albedo, surface.albedo);
            sum.emission = bx::add(sum.emission, surface.emission);
            sum.normal = bx::add(sum.normal, bx::add(bx::add(bx::mul(n[0], w), bx::mul(n[1], u)), bx::mul(n[2], v)));
            ++sum.samples;
        }
}

} // namespace

std::array<int, 3> VoxelGrid::coord(size_t index) const
{
    const size_t x = size_t(dims[0]), xy = x * size_t(dims[1]);
    return {int(index % x), int((index / x) % size_t(dims[1])), int(index / xy)};
}

bx::Vec3 VoxelGrid::voxelCenter(int i, int j, int k) const
{
    return bx::add(origin, bx::mul(bx::Vec3{float(i) + 0.5f, float(j) + 0.5f, float(k) + 0.5f}, voxelSize));
}

std::optional<std::array<int, 3>> VoxelGrid::voxelAt(const bx::Vec3& p) const
{
    const float local[3] = {(p.x - origin.x) / voxelSize, (p.y - origin.y) / voxelSize, (p.z - origin.z) / voxelSize};
    std::array<int, 3> cell{};
    for (int a = 0; a < 3; ++a) {
        if (!(local[a] >= 0.0f) || local[a] >= float(dims[a]))
            return std::nullopt;
        cell[a] = std::min(int(local[a]), dims[a] - 1);
    }
    return cell;
}

VoxelGrid voxelGridFor(const bx::Vec3& center, const bx::Vec3& boxSize, int resolution)
{
    VoxelGrid grid;
    grid.voxelSize = std::max({boxSize.x, boxSize.y, boxSize.z}) / float(std::max(resolution, 1));
    const float size[3] = {boxSize.x, boxSize.y, boxSize.z};
    for (int a = 0; a < 3; ++a)
        grid.dims[a] = std::clamp(int(std::ceil(size[a] / grid.voxelSize - 1e-3f)), 1, std::max(resolution, 1));
    grid.origin = bx::sub(center, bx::mul(grid.extent(), 0.5f));
    return grid;
}

uint8_t voxelGridMips(const VoxelGrid& grid)
{
    const int longest = std::max({grid.dims[0], grid.dims[1], grid.dims[2], 1});
    return static_cast<uint8_t>(std::bit_width(unsigned(longest)));
}

ProbeBox voxelGiBox(const VoxelGrid& grid, int priority)
{
    return {grid.center(), bx::mul(grid.extent(), 0.5f), priority};
}

float voxelGiEdgeWeight(const VoxelGrid& grid, const bx::Vec3& p)
{
    const bx::Vec3 hi = bx::add(grid.origin, grid.extent());
    const bx::Vec3 d = bx::min(bx::sub(p, grid.origin), bx::sub(hi, p));
    return std::clamp(std::min({d.x, d.y, d.z}) / (float(kVoxelGiEdgeVoxels) * grid.voxelSize), 0.0f, 1.0f);
}

VoxelGiPreset voxelGiPreset(VoxelGiQuality quality)
{
    switch (quality) {
    case VoxelGiQuality::Low:
        return {.cones = 4, .aperture = 0.8391f, .stepScale = 1.0f, .bounces = 1, .specularSteps = 32, .diffuseSteps = 12};
    case VoxelGiQuality::High:
        return {.cones = 9, .aperture = 0.41421f, .stepScale = 0.5f, .bounces = 2, .specularSteps = 128, .diffuseSteps = 24};
    case VoxelGiQuality::Medium:
        break;
    }
    return {};
}

std::vector<std::array<float, 4>> voxelGiCones(int count)
{
    count = std::clamp(count, 1, kMaxVoxelGiCones);
    std::vector<std::array<float, 4>> cones{{0.0f, 0.0f, 1.0f, 1.0f}};
    constexpr float kRing = 1.04719755f; // 60 degrees off the normal
    for (int i = 1; i < count; ++i) {
        const float phi = 6.28318531f * float(i - 1) / float(count - 1);
        cones.push_back({std::sin(kRing) * std::cos(phi), std::sin(kRing) * std::sin(phi), std::cos(kRing), std::cos(kRing)});
    }
    const float total = std::accumulate(cones.begin(), cones.end(), 0.0f, [](float s, const auto& c) { return s + c[3]; });
    for (auto& cone : cones)
        cone[3] /= total;
    return cones;
}

std::vector<ConeStep> voxelConeSteps(float aperture, float voxelSize, float stepScale, float maxDistance, int maxSteps)
{
    std::vector<ConeStep> steps;
    for (float t = voxelSize; int(steps.size()) < maxSteps && t < maxDistance;) {
        const float diameter = std::max(voxelSize, 2.0f * aperture * t);
        steps.push_back({t, diameter, std::log2(diameter / voxelSize)});
        t += diameter * stepScale;
    }
    return steps;
}

float voxelGiSpecularAperture(float roughness)
{
    const float r = std::clamp(roughness, 0.04f, 1.0f); // fs_mesh.sc's floor
    return std::max(r * r, kVoxelGiMinAperture);
}

VoxelGiLights packVoxelGiLights(const bx::Vec3* sunDirection, const bx::Vec3& sunColor, bool sunShadowed,
                                const std::vector<LocalLightSample>& lights, const std::vector<float>& shadows,
                                const Aabb& bounds)
{
    VoxelGiLights out;
    const auto slot = [&](size_t i) { return &out.data[i * 4]; };
    if (sunDirection && bx::dot(sunColor, sunColor) > 0.0f) {
        std::array<float, 4>* s = slot(out.count++);
        s[0] = {sunDirection->x, sunDirection->y, sunDirection->z, float(VoxelGiLightType::Directional)};
        s[1] = {sunColor.x, sunColor.y, sunColor.z, 0.0f};
        s[3] = {0.0f, 0.0f, sunShadowed ? 8.0f : -1.0f, 0.0f};
    }
    const bx::Vec3 center = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    std::vector<size_t> order;
    for (size_t i = 0; i < lights.size(); ++i)
        if (lightReaches(lights[i], bounds))
            order.push_back(i);
    std::ranges::stable_sort(order, {}, [&](size_t i) { return bx::length(bx::sub(lights[i].position, center)); });
    for (size_t i : order) {
        if (out.count == kMaxVoxelGiLights)
            break;
        const LocalLightSample& l = lights[i];
        std::array<float, 4>* s = slot(out.count++);
        const float shadow = i < shadows.size() ? shadows[i] : -1.0f;
        s[1] = {l.color.x, l.color.y, l.color.z, l.range};
        s[3] = {0.0f, 0.0f, shadow, l.bias};
        switch (l.type) {
        case LocalLightType::Point:
            s[0] = {l.position.x, l.position.y, l.position.z, float(VoxelGiLightType::Point)};
            break;
        case LocalLightType::Spot:
            s[0] = {l.position.x, l.position.y, l.position.z, float(VoxelGiLightType::Spot)};
            s[2] = {l.direction.x, l.direction.y, l.direction.z, l.cosOuter};
            s[3][0] = l.cosInner;
            break;
        case LocalLightType::Area:
            s[0] = {l.position.x, l.position.y, l.position.z,
                    float(l.twoSided ? VoxelGiLightType::AreaTwoSided : VoxelGiLightType::Area)};
            s[2] = {l.halfWidth.x, l.halfWidth.y, l.halfWidth.z, l.halfHeight.x};
            s[3][0] = l.halfHeight.y;
            s[3][1] = l.halfHeight.z;
            break;
        }
    }
    return out;
}

bx::Vec3 voxelGiLightIrradiance(const VoxelGiLights& lights, size_t light, const bx::Vec3& p, const bx::Vec3& n)
{
    const std::array<float, 4>* s = &lights.data[light * 4];
    const bx::Vec3 color{s[1][0], s[1][1], s[1][2]};
    const auto type = VoxelGiLightType(int(s[0][3]));
    if (type == VoxelGiLightType::Directional)
        return bx::mul(color, std::max(bx::dot(n, bx::neg({s[0][0], s[0][1], s[0][2]})), 0.0f));
    const bx::Vec3 to = bx::sub(bx::Vec3{s[0][0], s[0][1], s[0][2]}, p);
    const float d = bx::length(to);
    const float range = s[1][3];
    if (type == VoxelGiLightType::Area || type == VoxelGiLightType::AreaTwoSided) {
        const bx::Vec3 halfWidth{s[2][0], s[2][1], s[2][2]}, halfHeight{s[2][3], s[3][0], s[3][1]};
        const float ratio = d / range;
        const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
        const std::array<bx::Vec3, 4> corners = {bx::sub(bx::sub(to, halfWidth), halfHeight),
                                                 bx::sub(bx::add(to, halfWidth), halfHeight),
                                                 bx::add(bx::add(to, halfWidth), halfHeight),
                                                 bx::add(bx::sub(to, halfWidth), halfHeight)};
        return bx::mul(color, window * window * rectFormFactor(corners, n, type == VoxelGiLightType::AreaTwoSided));
    }
    const bx::Vec3 l = bx::mul(to, 1.0f / std::max(d, 1e-4f));
    float attenuation = pointLightFalloff(d, range) * std::max(bx::dot(n, l), 0.0f);
    if (type == VoxelGiLightType::Spot)
        attenuation *= spotConeFalloff(bx::dot(bx::neg(l), {s[2][0], s[2][1], s[2][2]}), s[3][0], s[2][3]);
    return bx::mul(color, attenuation);
}

std::string writeVoxels(const std::array<int, 3>& dims, const std::vector<Voxel>& voxels)
{
    std::string out(kVoxelsMagic, 4);
    put(out, uint32_t(kVoxelGiFormatVersion));
    for (int d : dims)
        put(out, uint32_t(d));
    put(out, uint32_t(voxels.size()));
    out.reserve(out.size() + voxels.size() * 16);
    for (const Voxel& v : voxels) {
        put(out, v.index);
        put(out, packRgba8(v.albedo.x, v.albedo.y, v.albedo.z, v.opacity));
        put(out, packRgba8(v.normal.x * 0.5f + 0.5f, v.normal.y * 0.5f + 0.5f, v.normal.z * 0.5f + 0.5f, 0.0f));
        put(out, packRgbe(v.emission));
    }
    return out;
}

bool readVoxels(std::string_view bytes, const std::array<int, 3>& dims, std::vector<Voxel>& out, std::string& error)
{
    size_t at = 4;
    uint32_t version = 0, count = 0;
    std::array<uint32_t, 3> stored{};
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kVoxelsMagic, 4) != 0 || !get(bytes, at, version)) {
        error = "not a .kvox voxel file";
        return false;
    }
    if (version != uint32_t(kVoxelGiFormatVersion)) {
        error = std::format("the .kvox is version {}, expected {} (bake again)", version, kVoxelGiFormatVersion);
        return false;
    }
    if (!get(bytes, at, stored[0]) || !get(bytes, at, stored[1]) || !get(bytes, at, stored[2]) || !get(bytes, at, count) ||
        bytes.size() - at != size_t(count) * 16) {
        error = "the .kvox is truncated (bake again)";
        return false;
    }
    if (stored[0] != uint32_t(dims[0]) || stored[1] != uint32_t(dims[1]) || stored[2] != uint32_t(dims[2])) {
        error = "the .kvox's grid doesn't match its .json (bake again)";
        return false;
    }
    const size_t total = size_t(dims[0]) * size_t(dims[1]) * size_t(dims[2]);
    std::vector<Voxel> voxels(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t albedo = 0, normal = 0, emission = 0;
        Voxel& v = voxels[i];
        get(bytes, at, v.index);
        get(bytes, at, albedo);
        get(bytes, at, normal);
        get(bytes, at, emission);
        if (v.index >= total || (i > 0 && v.index <= voxels[i - 1].index)) {
            error = "the .kvox's voxels are out of order or outside its grid (bake again)";
            return false;
        }
        v.albedo = {fromUnorm8(albedo), fromUnorm8(albedo >> 8), fromUnorm8(albedo >> 16)};
        v.opacity = fromUnorm8(albedo >> 24);
        v.normal = {fromUnorm8(normal) * 2.0f - 1.0f, fromUnorm8(normal >> 8) * 2.0f - 1.0f,
                    fromUnorm8(normal >> 16) * 2.0f - 1.0f};
        v.emission = unpackRgbe(emission);
    }
    out = std::move(voxels);
    return true;
}

std::string voxelGiSidecarJson(const VoxelGiSidecar& sidecar)
{
    const VoxelGrid& g = sidecar.grid;
    Json json;
    json["version"] = kVoxelGiFormatVersion;
    json["hash"] = sidecar.hash;
    json["resolution"] = sidecar.resolution;
    json["origin"] = {g.origin.x, g.origin.y, g.origin.z};
    json["voxelSize"] = g.voxelSize;
    json["dims"] = g.dims;
    json["voxels"] = sidecar.voxels;
    json["file"] = sidecar.file;
    json["fileHash"] = hex(sidecar.fileHash);
    return json.dump(2) + '\n';
}

bool parseVoxelGiSidecar(std::string_view text, VoxelGiSidecar& out, std::string& error)
{
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer()) {
        error = "the .json is not a VoxelGI sidecar";
        return false;
    }
    if (json["version"].get<int>() != kVoxelGiFormatVersion) {
        error = std::format("the .json is version {}, expected {} (bake again)", json["version"].dump(), kVoxelGiFormatVersion);
        return false;
    }
    const auto malformed = [&](const char* what) {
        error = std::format("the .json has a malformed \"{}\"", what);
        return false;
    };
    VoxelGiSidecar s;
    if (!json.contains("hash") || !json["hash"].is_string())
        return malformed("hash");
    s.hash = json["hash"].get<std::string>();
    if (!json.contains("resolution") || !json["resolution"].is_number_integer() ||
        std::ranges::find(kVoxelGiResolutions, json["resolution"].get<int>()) == std::end(kVoxelGiResolutions))
        return malformed("resolution");
    s.resolution = json["resolution"].get<int>();
    const Json& origin = json.value("origin", Json());
    if (!origin.is_array() || origin.size() != 3 || !std::ranges::all_of(origin, finite))
        return malformed("origin");
    s.grid.origin = {origin[0].get<float>(), origin[1].get<float>(), origin[2].get<float>()};
    if (!json.contains("voxelSize") || !finite(json["voxelSize"]) || !(json["voxelSize"].get<float>() > 0.0f))
        return malformed("voxelSize");
    s.grid.voxelSize = json["voxelSize"].get<float>();
    const Json& dims = json.value("dims", Json());
    if (!dims.is_array() || dims.size() != 3 ||
        !std::ranges::all_of(dims, [&](const Json& d) {
            return d.is_number_integer() && d.get<int>() >= 1 && d.get<int>() <= s.resolution;
        }))
        return malformed("dims");
    s.grid.dims = {dims[0].get<int>(), dims[1].get<int>(), dims[2].get<int>()};
    if (!json.contains("voxels") || !json["voxels"].is_number_unsigned() || json["voxels"].get<uint64_t>() > kMaxVoxelGiVoxels ||
        json["voxels"].get<uint64_t>() > s.grid.count())
        return malformed("voxels");
    s.voxels = json["voxels"].get<uint32_t>();
    if (!json.contains("file") || !json["file"].is_string() || json["file"].get<std::string>().empty() ||
        json["file"].get<std::string>().find_first_of("/\\") != std::string::npos)
        return malformed("file");
    s.file = json["file"].get<std::string>();
    if (!json.contains("fileHash") || !parseHex(json["fileHash"], s.fileHash))
        return malformed("fileHash");
    out = std::move(s);
    return true;
}

bool checkVoxelGiBake(std::string_view json, std::string_view kvox, int resolution, VoxelGiSidecar& sidecar,
                      std::vector<Voxel>& voxels, std::string& error)
{
    VoxelGiSidecar s;
    if (!parseVoxelGiSidecar(json, s, error))
        return false;
    if (s.resolution != resolution) {
        error = std::format("baked at resolution {}, the volume is {} (bake again)", s.resolution, resolution);
        return false;
    }
    if (lightmapFileHash(kvox) != s.fileHash) {
        error = std::format("'{}' doesn't match its .json (bake again)", s.file);
        return false;
    }
    std::vector<Voxel> read;
    if (!readVoxels(kvox, s.grid.dims, read, error))
        return false;
    if (read.size() != s.voxels) {
        error = std::format("'{}' holds {} voxels, its .json {} (bake again)", s.file, read.size(), s.voxels);
        return false;
    }
    sidecar = std::move(s);
    voxels = std::move(read);
    return true;
}

std::string voxelGiSidecarPath(const std::string& scenePath, EntityId id)
{
    const std::filesystem::path scene(reinterpret_cast<const char8_t*>(scenePath.c_str()));
    const std::filesystem::path file =
        scene.parent_path() / (scene.stem().u8string() + u8"_voxelgi") / std::filesystem::path(std::format("{}.json", id));
    const std::u8string text = file.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string voxelGiSceneHash(const Scene& scene, entt::entity volume)
{
    const entt::registry& registry = scene.registry();
    std::map<EntityId, std::string> parts; // by id: the order entities were created in doesn't count
    const auto matrix = [&](entt::entity entity) {
        float world[16];
        scene.worldMatrix(entity, world);
        std::string s;
        for (float v : world)
            s += std::format("{:.5g},", v);
        return s;
    };
    const auto id = [&](entt::entity entity) { return registry.get<PersistentId>(entity).value; };
    for (auto [entity, mesh] : registry.view<MeshRenderer>().each())
        if (mesh.giMode == GiMode::Static)
            parts[id(entity)] += std::format("mesh {} {} {}|", int(mesh.primitive), mesh.material, matrix(entity));
    for (auto [entity, model] : registry.view<ModelRenderer>().each()) {
        if (model.giMode != GiMode::Static)
            continue;
        std::string materials;
        for (const std::string& m : model.materials)
            materials += m + ";";
        parts[id(entity)] += std::format("model {} {} {}|", model.model, materials, matrix(entity));
    }
    for (auto [entity, terrain] : registry.view<Terrain>().each())
        if (terrain.lightmap)
            parts[id(entity)] += std::format("terrain {}|", terrain.map);
    std::string all;
    for (const auto& [key, text] : parts)
        all += std::format("{}:{}\n", key, text);
    const VoxelGI& v = registry.get<VoxelGI>(volume);
    float world[16];
    scene.worldMatrix(volume, world);
    all += std::format("volume {:.5g},{:.5g},{:.5g} {:.5g},{:.5g},{:.5g} {}", world[12], world[13], world[14], v.boxSize.x,
                       v.boxSize.y, v.boxSize.z, v.resolution);
    return hex(lightmapFileHash(all));
}

bool gatherVoxelGiBake(const Scene& scene, const std::filesystem::path& root, const std::string& scenePath,
                       entt::entity volume, VoxelGiBakeInput& out, std::string& error)
{
    const entt::registry& registry = scene.registry();
    const VoxelGI* component = scene.valid(volume) ? registry.try_get<VoxelGI>(volume) : nullptr;
    if (!component || scenePath.empty()) {
        error = "VoxelGI bakes a VoxelGI volume of a scene saved in the project";
        return false;
    }
    VoxelGiBakeInput input;
    input.id = registry.get<PersistentId>(volume).value;
    input.name = registry.get<Name>(volume).value;
    input.resolution = component->resolution;
    float world[16];
    scene.worldMatrix(volume, world);
    input.grid = voxelGridFor({world[12], world[13], world[14]}, component->boxSize, component->resolution);
    input.hash = voxelGiSceneHash(scene, volume);
    input.bakedPath = voxelGiSidecarPath(scenePath, input.id);
    input.json = root / std::filesystem::path(reinterpret_cast<const char8_t*>(input.bakedPath.c_str()));
    std::string nothing;
    if (!gatherLightmapBake(scene, root, scenePath, input.scene, nothing))
        input.scene = {}; // no Static geometry: an empty volume
    out = std::move(input);
    return true;
}

std::vector<Voxel> voxelizeVoxelGi(const VoxelGiBakeInput& input, LightmapBakeProgress& progress)
{
    const VoxelGrid& grid = input.grid;
    std::vector<int32_t> slot(grid.count(), -1);
    std::vector<VoxelSum> sums;
    const LightmapBakeInput& scene = input.scene;
    const size_t total = scene.entities.size() + scene.terrain.size();
    size_t done = 0;
    for (const BakeEntity& entity : scene.entities) {
        if (progress.cancel.load())
            return {};
        for (const BakeMesh& mesh : entity.meshes) {
            const BakeSurface& surface = scene.surfaces[mesh.surface];
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const auto point = [&](size_t c) { return transformPoint(mesh.positions[mesh.indices[i + c]], entity.world); };
                const auto normal = [&](size_t c) { return transformNormal(mesh.normals[mesh.indices[i + c]], entity.world); };
                voxelizeTriangle(grid, {point(0), point(1), point(2)}, {normal(0), normal(1), normal(2)}, surface, slot, sums);
            }
        }
        progress.fraction.store(float(++done) / float(std::max<size_t>(total, 1)));
    }
    for (const BakeTerrain& terrain : scene.terrain) {
        if (progress.cancel.load())
            return {};
        const BakeSurface& surface = scene.surfaces[terrain.surface];
        for (const BakeTerrainTile& tile : terrain.tiles)
            map::for_each_triangle([&](uint32_t a, uint32_t b, uint32_t c) {
                voxelizeTriangle(grid, {tile.positions[a], tile.positions[b], tile.positions[c]},
                                 {tile.normals[a], tile.normals[b], tile.normals[c]}, surface, slot, sums);
            });
        progress.fraction.store(float(++done) / float(std::max<size_t>(total, 1)));
    }
    std::vector<Voxel> voxels;
    voxels.reserve(sums.size());
    for (size_t index = 0; index < slot.size(); ++index) {
        if (slot[index] < 0)
            continue;
        const VoxelSum& sum = sums[static_cast<size_t>(slot[index])];
        const float inv = 1.0f / float(sum.samples);
        const float length = bx::length(sum.normal);
        voxels.push_back({.index = static_cast<uint32_t>(index),
                          .albedo = bx::mul(sum.albedo, inv),
                          .opacity = 1.0f,
                          .normal = length > 1e-6f ? bx::mul(sum.normal, 1.0f / length) : bx::Vec3{0.0f, 0.0f, 0.0f},
                          .emission = bx::mul(sum.emission, inv)});
    }
    return voxels;
}

bool bakeVoxelGi(const VoxelGiBakeInput& input, LightmapBakeProgress& progress, VoxelGiBakeOutput& out, std::string& error)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<Voxel> voxels = voxelizeVoxelGi(input, progress);
    if (progress.cancel.load()) {
        error = "cancelled";
        return false;
    }
    if (voxels.size() > kMaxVoxelGiVoxels) {
        error = std::format("{} voxels, more than {}: lower the resolution or shrink the box", voxels.size(),
                            kMaxVoxelGiVoxels);
        return false;
    }
    VoxelGiBakeOutput result;
    VoxelGiSidecar sidecar{.hash = input.hash,
                           .resolution = input.resolution,
                           .grid = input.grid,
                           .voxels = static_cast<uint32_t>(voxels.size()),
                           .file = std::format("{}.kvox", input.id)};
    std::string kvox = writeVoxels(input.grid.dims, voxels);
    sidecar.fileHash = lightmapFileHash(kvox);
    result.files.emplace_back(input.json.parent_path() / std::filesystem::path(sidecar.file), std::move(kvox));
    result.files.emplace_back(input.json, voxelGiSidecarJson(sidecar));
    result.voxels = voxels.size();
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    progress.fraction.store(1.0f);
    out = std::move(result);
    return true;
}

// --- GPU ---

void VoxelGiRenderer::init()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    const uint16_t format = caps->formats[bgfx::TextureFormat::RGBA16F];
    if (!(caps->supported & BGFX_CAPS_COMPUTE) || !(caps->supported & BGFX_CAPS_TEXTURE_3D) ||
        !(format & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE) || !(format & BGFX_CAPS_FORMAT_TEXTURE_3D)) {
        log::warn("VoxelGI: this GPU lacks compute or RGBA16F 3D images; VoxelGI volumes are off");
        return;
    }
    inject_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_voxelgi_inject, sizeof(cs_voxelgi_inject))), true);
    mip_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_voxelgi_mip, sizeof(cs_voxelgi_mip))), true);
    bounce_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_voxelgi_bounce, sizeof(cs_voxelgi_bounce))), true);
    params_ = bgfx::createUniform("u_voxel", bgfx::UniformType::Vec4, 4);
    lights_ = bgfx::createUniform("u_voxelLights", bgfx::UniformType::Vec4, kMaxVoxelGiLights * 4);
    cones_ = bgfx::createUniform("u_voxelCones", bgfx::UniformType::Vec4, kMaxVoxelGiCones);
    csmMtx_ = bgfx::createUniform("u_csmMtx", bgfx::UniformType::Mat4, kMaxShadowCascades + kMaxShadowedLocalLights);
    csmBias_ = bgfx::createUniform("u_csmBias", bgfx::UniformType::Vec4, kMaxShadowCascades);
    csmSampler_ = bgfx::createUniform("s_csm", bgfx::UniformType::Sampler);
    sourceSampler_ = bgfx::createUniform("s_voxelSource", bgfx::UniformType::Sampler);
    for (size_t i = 0; i < kMaxShadowedPointLights; ++i)
        cubeSamplers_[i] = bgfx::createUniform(std::format("s_pointShadow{}", i).c_str(), bgfx::UniformType::Sampler);
    // 1x1 stand-ins for the shadow maps not made yet: D3D11 wants a comparison sampler where the shader declares one.
    constexpr uint64_t kShadowFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | BGFX_SAMPLER_UVW_CLAMP;
    noShadow2D_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::D16, kShadowFlags);
    noShadowCube_ = bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::D16, kShadowFlags);
    layout_.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
}

void VoxelGiRenderer::shutdown()
{
    if (!supported())
        return;
    for (bgfx::ProgramHandle program : {inject_, mip_, bounce_})
        bgfx::destroy(program);
    for (bgfx::UniformHandle uniform : {params_, lights_, cones_, csmMtx_, csmBias_, csmSampler_, sourceSampler_})
        bgfx::destroy(uniform);
    for (bgfx::UniformHandle uniform : cubeSamplers_)
        bgfx::destroy(uniform);
    bgfx::destroy(noShadow2D_);
    bgfx::destroy(noShadowCube_);
    inject_ = BGFX_INVALID_HANDLE;
}

bool VoxelGiRenderer::create(Volume& volume, const VoxelGrid& grid, const std::vector<Voxel>& voxels)
{
    if (!supported())
        return false;
    volume.grid = grid;
    volume.voxels = static_cast<uint32_t>(voxels.size());
    volume.mips = voxelGridMips(grid);
    // At least one voxel's worth of buffer: an empty bake still binds them.
    const uint32_t count = std::max(volume.voxels, 1u);
    const bgfx::Memory* data = bgfx::alloc(count * 4 * 16);
    std::memset(data->data, 0, data->size);
    auto* out = reinterpret_cast<float*>(data->data);
    for (const Voxel& v : voxels) {
        const std::array<int, 3> c = grid.coord(v.index);
        const float values[16] = {float(c[0]), float(c[1]), float(c[2]), v.opacity, v.albedo.x, v.albedo.y, v.albedo.z,
                                  0.0f, v.normal.x, v.normal.y, v.normal.z, 0.0f, v.emission.x, v.emission.y,
                                  v.emission.z, 0.0f};
        std::memcpy(out, values, sizeof(values));
        out += 16;
    }
    volume.data = bgfx::createVertexBuffer(data, layout_, BGFX_BUFFER_COMPUTE_READ);
    volume.direct = bgfx::createDynamicVertexBuffer(count, layout_, BGFX_BUFFER_COMPUTE_READ_WRITE);
    // Zeroed once: the empty voxels are never written, the voxels and mips every frame.
    bgfx::TextureInfo info;
    bgfx::calcTextureSize(info, uint16_t(grid.dims[0]), uint16_t(grid.dims[1]), uint16_t(grid.dims[2]), false, true, 1,
                          bgfx::TextureFormat::RGBA16F);
    for (bgfx::TextureHandle& texture : volume.radiance) {
        const bgfx::Memory* zero = bgfx::alloc(info.storageSize);
        std::memset(zero->data, 0, zero->size);
        texture = bgfx::createTexture3D(uint16_t(grid.dims[0]), uint16_t(grid.dims[1]), uint16_t(grid.dims[2]), true,
                                        bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_UVW_CLAMP,
                                        zero);
    }
    return true;
}

void VoxelGiRenderer::destroy(Volume& volume)
{
    if (bgfx::isValid(volume.data))
        bgfx::destroy(volume.data);
    if (bgfx::isValid(volume.direct))
        bgfx::destroy(volume.direct);
    for (bgfx::TextureHandle texture : volume.radiance)
        if (bgfx::isValid(texture))
            bgfx::destroy(texture);
    volume = {};
}

void VoxelGiRenderer::mips(bgfx::ViewId view, const Volume& volume, bgfx::TextureHandle texture)
{
    std::array<int, 3> size = volume.grid.dims;
    for (uint8_t level = 1; level < volume.mips; ++level) {
        const std::array<int, 3> next{std::max(size[0] / 2, 1), std::max(size[1] / 2, 1), std::max(size[2] / 2, 1)};
        const float params[4][4] = {{float(size[0]), float(size[1]), float(size[2]), 0.0f},
                                    {float(next[0]), float(next[1]), float(next[2]), 0.0f}};
        bgfx::setUniform(params_, params, 4);
        bgfx::setImage(0, texture, uint8_t(level - 1), bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
        bgfx::setImage(1, texture, level, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
        bgfx::dispatch(view, mip_, uint32_t(next[0] + 3) / 4, uint32_t(next[1] + 3) / 4, uint32_t(next[2] + 3) / 4);
        size = next;
    }
}

bgfx::TextureHandle VoxelGiRenderer::update(bgfx::ViewId view, const Volume& volume, const VoxelGiPreset& preset,
                                            const VoxelGiLights& lights, const Shadows& shadows)
{
    const VoxelGrid& grid = volume.grid;
    const uint32_t groups = (volume.voxels + 63) / 64;
    const bx::Vec3 extent = grid.extent();
    const float origin[4] = {grid.origin.x, grid.origin.y, grid.origin.z, grid.voxelSize};

    const float inject[4][4] = {{origin[0], origin[1], origin[2], origin[3]},
                                {float(volume.voxels), float(lights.count), float(shadows.cascades), 0.0f}};
    bgfx::setUniform(params_, inject, 4);
    bgfx::setUniform(lights_, lights.data.data(), uint16_t(kMaxVoxelGiLights * 4));
    if (shadows.csmMtx)
        bgfx::setUniform(csmMtx_, shadows.csmMtx, kMaxShadowCascades + kMaxShadowedLocalLights);
    if (shadows.csmBias)
        bgfx::setUniform(csmBias_, shadows.csmBias, kMaxShadowCascades);
    bgfx::setBuffer(0, volume.data, bgfx::Access::Read);
    bgfx::setBuffer(1, volume.direct, bgfx::Access::ReadWrite);
    bgfx::setImage(2, volume.radiance[0], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    bgfx::setTexture(3, csmSampler_, bgfx::isValid(shadows.atlas) ? shadows.atlas : noShadow2D_);
    for (size_t cube = 0; cube < kMaxShadowedPointLights; ++cube)
        bgfx::setTexture(uint8_t(4 + cube), cubeSamplers_[cube],
                         bgfx::isValid(shadows.cubes[cube]) ? shadows.cubes[cube] : noShadowCube_);
    if (groups > 0)
        bgfx::dispatch(view, inject_, groups);
    mips(view, volume, volume.radiance[0]);

    std::array<float, 4> cones[kMaxVoxelGiCones] = {};
    const std::vector<std::array<float, 4>> set = voxelGiCones(preset.cones);
    std::copy(set.begin(), set.end(), cones);
    size_t source = 0;
    for (int bounce = 0; bounce < preset.bounces; ++bounce) {
        const float params[4][4] = {{origin[0], origin[1], origin[2], origin[3]},
                                    {float(volume.voxels), 0.0f, 0.0f, 0.0f},
                                    {1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z, float(volume.mips - 1)},
                                    {float(set.size()), preset.aperture, preset.stepScale, float(preset.diffuseSteps)}};
        bgfx::setUniform(params_, params, 4);
        bgfx::setUniform(cones_, cones, kMaxVoxelGiCones);
        bgfx::setBuffer(0, volume.data, bgfx::Access::Read);
        bgfx::setBuffer(1, volume.direct, bgfx::Access::Read);
        bgfx::setTexture(2, sourceSampler_, volume.radiance[source]);
        bgfx::setImage(3, volume.radiance[1 - source], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
        if (groups > 0)
            bgfx::dispatch(view, bounce_, groups);
        source = 1 - source;
        mips(view, volume, volume.radiance[source]);
    }
    return volume.radiance[source];
}

} // namespace kast
