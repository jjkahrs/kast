#include "sdfgi.h"

#include "cs_sdfgi_build.bin.h"
#include "cs_sdfgi_light.bin.h"
#include "cs_sdfgi_probe.bin.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>

namespace kast {
namespace {

bool loggingOn = false;

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int wrap(int a, int m) { return ((a % m) + m) % m; }

std::array<float, 25> shapeKey(const SdfgiShape& s)
{
    std::array<float, 25> k{};
    size_t i = 0;
    for (const auto& row : s.rows)
        for (float v : row)
            k[i++] = v;
    for (const bx::Vec3& v : {s.half, s.albedo, s.emission}) {
        k[i++] = v.x;
        k[i++] = v.y;
        k[i++] = v.z;
    }
    k[i++] = float(s.type);
    return k;
}

bool overlaps(const Aabb& a, const Aabb& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z &&
           a.max.z >= b.min.z;
}

} // namespace

CellBox intersect(const CellBox& a, const CellBox& b)
{
    CellBox out;
    for (int i = 0; i < 3; ++i) {
        out.min[i] = std::max(a.min[i], b.min[i]);
        out.max[i] = std::min(a.max[i], b.max[i]);
    }
    return out;
}

std::array<int, 3> SdfgiCascade::texel(const std::array<int, 3>& c) const
{
    return {wrap(c[0], dims[0]), wrap(c[1], dims[1]), wrap(c[2], dims[2])};
}

std::array<int, 3> SdfgiCascade::cellAt(const bx::Vec3& p) const
{
    return {int(std::floor(p.x / cellSize)), int(std::floor(p.y / cellSize)), int(std::floor(p.z / cellSize))};
}

std::array<int, 3> sdfgiDims(float yScale)
{
    return {kSdfgiCells, std::max(kSdfgiProbeCells, int(std::lround(float(kSdfgiCells) * yScale))), kSdfgiCells};
}

SdfgiCascade sdfgiCascadeAt(const bx::Vec3& eye, int cascade, float cellSize, float yScale)
{
    SdfgiCascade out;
    out.dims = sdfgiDims(yScale);
    out.cellSize = cellSize * float(1 << cascade);
    const std::array<int, 3> at = out.cellAt(eye);
    for (int i = 0; i < 3; ++i)
        out.origin[i] = floorDiv(at[i] - out.dims[i] / 2, kSdfgiProbeCells) * kSdfgiProbeCells;
    return out;
}

std::vector<CellBox> sdfgiScrollRegions(const SdfgiCascade& from, const SdfgiCascade& to)
{
    if (from.cellSize != to.cellSize || from.dims != to.dims || intersect(from.box(), to.box()).empty())
        return {to.box()};
    std::vector<CellBox> out;
    CellBox rest = to.box();
    const CellBox old = from.box();
    for (int a = 0; a < 3; ++a) {
        if (rest.min[a] < old.min[a]) {
            CellBox slab = rest;
            slab.max[a] = old.min[a];
            out.push_back(slab);
            rest.min[a] = old.min[a];
        }
        if (rest.max[a] > old.max[a]) {
            CellBox slab = rest;
            slab.min[a] = old.max[a];
            out.push_back(slab);
            rest.max[a] = old.max[a];
        }
    }
    return out;
}

CellBox sdfgiCellsNear(const SdfgiCascade& cascade, const Aabb& bounds, float margin)
{
    const float s = cascade.cellSize;
    const CellBox near{{int(std::floor(bounds.min.x / s - margin)), int(std::floor(bounds.min.y / s - margin)),
                        int(std::floor(bounds.min.z / s - margin))},
                       {int(std::floor(bounds.max.x / s + margin)) + 1, int(std::floor(bounds.max.y / s + margin)) + 1,
                        int(std::floor(bounds.max.z / s + margin)) + 1}};
    return intersect(near, cascade.box());
}

float sdfgiHeightfieldDistance(float y, float height, float slope)
{
    return (y - height) / std::sqrt(1.0f + slope * slope);
}

std::vector<std::array<float, 2>> sdfgiTerrainColumns(const SdfgiCascade& cascade, const SdfgiHeightAt& heightAt)
{
    const int nx = cascade.dims[0], nz = cascade.dims[2];
    std::vector<std::array<float, 2>> out(size_t(nx) * size_t(nz), {kSdfgiNoTerrain, 0.0f});
    if (!heightAt)
        return out;
    const float s = cascade.cellSize;
    for (int z = cascade.origin[2]; z < cascade.origin[2] + nz; ++z)
        for (int x = cascade.origin[0]; x < cascade.origin[0] + nx; ++x) {
            const float wx = (float(x) + 0.5f) * s, wz = (float(z) + 0.5f) * s;
            const std::optional<float> h = heightAt(wx, wz);
            if (!h)
                continue;
            // The slope from the heights a cell either side (the center's where there is none).
            const auto at = [&](float px, float pz) { return heightAt(px, pz).value_or(*h); };
            const float dx = (at(wx + s, wz) - at(wx - s, wz)) / (2.0f * s);
            const float dz = (at(wx, wz + s) - at(wx, wz - s)) / (2.0f * s);
            out[size_t(wrap(z, nz)) * size_t(nx) + size_t(wrap(x, nx))] = {*h, std::sqrt(dx * dx + dz * dz)};
        }
    return out;
}

SdfgiProbeWeights sdfgiProbeWeights(const SdfgiCascade& cascade, const bx::Vec3& p, const bx::Vec3& n,
                                    const std::function<bool(int corner)>& valid)
{
    const float spacing = cascade.probeSpacing();
    const std::array<int, 3> dims = cascade.probeDims();
    const bx::Vec3 q = bx::add(p, bx::mul(n, kSdfgiNormalBias * spacing));
    const float qv[3] = {q.x, q.y, q.z};
    SdfgiProbeWeights out;
    float f[3];
    for (int a = 0; a < 3; ++a) {
        const float first = float(cascade.origin[a] / kSdfgiProbeCells);
        const float local = std::clamp(qv[a] / spacing - first, 0.0f, float(dims[a]) - 1.001f);
        const float base = std::floor(local);
        out.base[a] = int(first + base);
        f[a] = local - base;
    }
    float total = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const int corner[3] = {i & 1, i >> 1 & 1, i >> 2 & 1};
        if (!valid(i))
            continue;
        float tri = 1.0f;
        for (int a = 0; a < 3; ++a)
            tri *= corner[a] ? f[a] : 1.0f - f[a];
        const bx::Vec3 probe = bx::mul(bx::Vec3{float(out.base[0] + corner[0]), float(out.base[1] + corner[1]),
                                                float(out.base[2] + corner[2])},
                                       spacing);
        const bx::Vec3 toProbe = bx::normalize(bx::add(bx::sub(probe, p), bx::Vec3{1e-5f, 1e-5f, 1e-5f}));
        const float back = (bx::dot(toProbe, n) + 1.0f) * 0.5f;
        out.weights[size_t(i)] = tri * (back * back + 0.05f);
        total += out.weights[size_t(i)];
    }
    for (float& w : out.weights)
        w = total > 0.0f ? w / total : 0.0f;
    return out;
}

float sdfgiCascadeWeight(const SdfgiCascade& cascade, const bx::Vec3& p)
{
    const float spacing = cascade.probeSpacing();
    const std::array<int, 3> dims = cascade.probeDims();
    const float pv[3] = {p.x, p.y, p.z};
    float edge = FLT_MAX;
    for (int a = 0; a < 3; ++a) {
        const float local = pv[a] / spacing - float(cascade.origin[a] / kSdfgiProbeCells);
        edge = std::min({edge, local, float(dims[a] - 1) - local});
    }
    return std::clamp(edge / kSdfgiBlendProbes, 0.0f, 1.0f);
}

SdfgiPreset sdfgiPreset(SdfgiQuality quality)
{
    switch (quality) {
    case SdfgiQuality::Low:
        return {.rays = 16, .steps = 32, .hysteresis = 0.95f};
    case SdfgiQuality::High:
        return {.rays = 64, .steps = 64, .hysteresis = 0.9f};
    case SdfgiQuality::Medium:
        break;
    }
    return {};
}

const char* validateSdfgi(const SceneSdfgi& s)
{
    if (s.cascades < 1 || s.cascades > kMaxSdfgiCascades)
        return "cascades must be 1 to 4";
    if (!(s.cellSize >= kMinSdfgiCellSize && s.cellSize <= kMaxSdfgiCellSize))
        return "cellSize must be between 0.125 and 4";
    if (!(s.energy >= 0.0f && std::isfinite(s.energy)))
        return "energy must be >= 0";
    if (!(s.bounceFeedback >= 0.0f && s.bounceFeedback <= 1.0f))
        return "bounceFeedback must be between 0 and 1";
    if (std::ranges::find(kSdfgiYScales, s.yScale) == std::end(kSdfgiYScales))
        return "yScale must be 1, 0.5 or 0.25";
    return nullptr;
}

bool SdfgiShape::operator==(const SdfgiShape& o) const { return shapeKey(*this) == shapeKey(o); }
bool SdfgiShape::operator<(const SdfgiShape& o) const { return shapeKey(*this) < shapeKey(o); }

SdfgiShape sdfgiShape(SdfgiShapeType type, const float* world, const bx::Vec3& localHalf, const bx::Vec3& albedo,
                      const bx::Vec3& emission)
{
    SdfgiShape s;
    s.type = type;
    s.albedo = albedo;
    s.emission = emission;
    const bx::Vec3 t{world[12], world[13], world[14]};
    const float halfIn[3] = {localHalf.x, localHalf.y, localHalf.z};
    float half[3];
    bx::Vec3 extent{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; ++i) {
        const bx::Vec3 axis{world[i * 4], world[i * 4 + 1], world[i * 4 + 2]};
        const float scale = bx::length(axis);
        const bx::Vec3 unit{i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f}; // a zero-scale axis
        const bx::Vec3 u = scale > 0.0f ? bx::mul(axis, 1.0f / scale) : unit;
        half[i] = std::max(halfIn[i] * scale, kSdfgiPlaneHalf);
        s.rows[size_t(i)] = {u.x, u.y, u.z, -bx::dot(u, t)};
        extent = bx::add(extent, bx::mul(bx::Vec3{std::abs(u.x), std::abs(u.y), std::abs(u.z)}, half[i]));
    }
    s.half = {half[0], half[1], half[2]};
    s.bounds = {bx::sub(t, extent), bx::add(t, extent)};
    return s;
}

SdfgiShape sdfgiBoxShape(const Aabb& bounds, const bx::Vec3& albedo, const bx::Vec3& emission)
{
    const bx::Vec3 center = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    float world[16];
    bx::mtxTranslate(world, center.x, center.y, center.z);
    return sdfgiShape(SdfgiShapeType::Box, world, bx::mul(bx::sub(bounds.max, bounds.min), 0.5f), albedo, emission);
}

float sdfgiShapeDistance(const SdfgiShape& s, const bx::Vec3& p)
{
    const bx::Vec3 q{s.rows[0][0] * p.x + s.rows[0][1] * p.y + s.rows[0][2] * p.z + s.rows[0][3],
                     s.rows[1][0] * p.x + s.rows[1][1] * p.y + s.rows[1][2] * p.z + s.rows[1][3],
                     s.rows[2][0] * p.x + s.rows[2][1] * p.y + s.rows[2][2] * p.z + s.rows[2][3]};
    const bx::Vec3& h = s.half;
    if (s.type == SdfgiShapeType::Box) {
        const bx::Vec3 d{std::abs(q.x) - h.x, std::abs(q.y) - h.y, std::abs(q.z) - h.z};
        const bx::Vec3 outside{std::max(d.x, 0.0f), std::max(d.y, 0.0f), std::max(d.z, 0.0f)};
        return bx::length(outside) + std::min(std::max(d.x, std::max(d.y, d.z)), 0.0f);
    }
    const float k0 = bx::length(bx::Vec3{q.x / h.x, q.y / h.y, q.z / h.z});
    const float k1 = bx::length(bx::Vec3{q.x / (h.x * h.x), q.y / (h.y * h.y), q.z / (h.z * h.z)});
    return k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -std::min({h.x, h.y, h.z});
}

void setSdfgiLogging(bool on) { loggingOn = on; }
bool sdfgiLogging() { return loggingOn; }

// --- GPU ---

void SdfgiRenderer::init()
{
    // The meshes' uniform and sampler exist either way: every draw sets them (zeros: no SDFGI).
    meshUniform_ = bgfx::createUniform("u_sdfgi", bgfx::UniformType::Vec4, 6);
    meshSampler_ = bgfx::createUniform("s_sdfgi", bgfx::UniformType::Sampler);
    const bgfx::Caps* caps = bgfx::getCaps();
    const auto can = [&](bgfx::TextureFormat::Enum format, uint16_t needs) {
        return (caps->formats[format] & needs) == needs;
    };
    constexpr uint16_t kImage3D =
        BGFX_CAPS_FORMAT_TEXTURE_3D | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_READ;
    if (!(caps->supported & BGFX_CAPS_COMPUTE) || !(caps->supported & BGFX_CAPS_TEXTURE_3D) ||
        !can(bgfx::TextureFormat::RGBA16F, kImage3D) || !can(bgfx::TextureFormat::RGBA8, kImage3D) ||
        !can(bgfx::TextureFormat::RGBA32F, BGFX_CAPS_FORMAT_TEXTURE_2D | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_READ))
        return; // warned when a scene turns SDFGI on (Renderer::render)
    build_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_sdfgi_build, sizeof(cs_sdfgi_build))), true);
    light_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_sdfgi_light, sizeof(cs_sdfgi_light))), true);
    probe_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_sdfgi_probe, sizeof(cs_sdfgi_probe))), true);
    cascadesUniform_ = bgfx::createUniform("u_sdfgiCascades", bgfx::UniformType::Vec4, kMaxSdfgiCascades);
    gridUniform_ = bgfx::createUniform("u_sdfgiGrid", bgfx::UniformType::Vec4);
    buildUniform_ = bgfx::createUniform("u_sdfgiBuild", bgfx::UniformType::Vec4, 4);
    paramsUniform_ = bgfx::createUniform("u_sdfgiParams", bgfx::UniformType::Vec4, 2);
    lightsUniform_ = bgfx::createUniform("u_sdfgiLights", bgfx::UniformType::Vec4, kMaxVoxelGiLights * 4);
    skyUniform_ = bgfx::createUniform("u_sdfgiSky", bgfx::UniformType::Vec4, 9);
    rotationUniform_ = bgfx::createUniform("u_sdfgiRotation", bgfx::UniformType::Vec4, 3);
    layout_.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
}

void SdfgiRenderer::shutdown()
{
    destroyTextures();
    for (bgfx::UniformHandle uniform : {meshUniform_, meshSampler_})
        if (bgfx::isValid(uniform))
            bgfx::destroy(uniform);
    if (!supported())
        return;
    for (bgfx::ProgramHandle program : {build_, light_, probe_})
        bgfx::destroy(program);
    for (bgfx::UniformHandle uniform : {cascadesUniform_, gridUniform_, buildUniform_, paramsUniform_, lightsUniform_,
                                        skyUniform_, rotationUniform_})
        bgfx::destroy(uniform);
    build_ = BGFX_INVALID_HANDLE;
}

void SdfgiRenderer::destroyTextures()
{
    for (bgfx::TextureHandle* texture : {&albedo_, &sdf_, &lightVolume_, &terrain_, &probes_[0], &probes_[1]})
        if (bgfx::isValid(*texture)) {
            bgfx::destroy(*texture);
            *texture = BGFX_INVALID_HANDLE;
        }
    if (bgfx::isValid(shapes_))
        bgfx::destroy(shapes_);
    shapes_ = BGFX_INVALID_HANDLE;
}

void SdfgiRenderer::reset()
{
    destroyTextures();
    cascades_.clear();
    shapesLast_.clear();
    tilesLast_.clear();
    pending_.clear();
}

void SdfgiRenderer::create(const SceneSdfgi& settings)
{
    reset();
    settings_ = settings;
    const std::array<int, 3> dims = sdfgiDims(settings.yScale);
    const auto count = uint16_t(settings.cascades);
    constexpr uint64_t kFlags = BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP;
    const auto volume = [&](int x, bgfx::TextureFormat::Enum format) {
        return bgfx::createTexture3D(uint16_t(x), uint16_t(dims[1]), uint16_t(dims[2] * count), false, format, kFlags);
    };
    albedo_ = volume(dims[0], bgfx::TextureFormat::RGBA8);
    sdf_ = volume(dims[0], bgfx::TextureFormat::RGBA16F);
    lightVolume_ = volume(dims[0], bgfx::TextureFormat::RGBA16F);
    terrain_ = bgfx::createTexture2D(uint16_t(dims[0]), uint16_t(dims[2] * count), false, 1, bgfx::TextureFormat::RGBA32F,
                                     BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
    // The probes start with no history (a = -1), so the first update takes their rays whole.
    const int px = dims[0] / kSdfgiProbeCells * 4, py = dims[1] / kSdfgiProbeCells, pz = dims[2] / kSdfgiProbeCells * count;
    for (bgfx::TextureHandle& probes : probes_) {
        const bgfx::Memory* none = bgfx::alloc(uint32_t(px * py * pz) * 8);
        auto* half = reinterpret_cast<uint16_t*>(none->data);
        for (int i = 0; i < px * py * pz; ++i)
            for (int k = 0; k < 4; ++k)
                half[i * 4 + k] = k == 3 && i % 4 == 0 ? bx::halfFromFloat(-1.0f) : bx::halfFromFloat(0.0f);
        probes = bgfx::createTexture3D(uint16_t(px), uint16_t(py), uint16_t(pz), false, bgfx::TextureFormat::RGBA16F,
                                       kFlags, none);
    }
    shapes_ = bgfx::createDynamicVertexBuffer(256 * 6, layout_, BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_ALLOW_RESIZE);
}

void SdfgiRenderer::update(bgfx::ViewId view, const Frame& frame)
{
    if (!supported())
        return;
    const SceneSdfgi& s = frame.settings;
    if (cascades_.empty() || s.cascades != settings_.cascades || s.cellSize != settings_.cellSize ||
        s.yScale != settings_.yScale || !bgfx::isValid(sdf_)) {
        if (!cascades_.empty())
            log::info("SDFGI: settings changed; building every cascade again");
        create(s);
    }
    energy_ = s.energy;
    const SdfgiPreset preset = sdfgiPreset(s.quality);
    const bool first = cascades_.empty();

    // Where the cascades go this frame, and what of them needs building.
    std::vector<SdfgiCascade> next;
    for (int i = 0; i < s.cascades; ++i)
        next.push_back(sdfgiCascadeAt(frame.eye, i, s.cellSize, s.yScale));
    std::vector<Dirty> dirty = std::exchange(pending_, {});
    for (Dirty& d : dirty) // waited a frame: what of it is still in its (maybe scrolled) cascade
        d.box = intersect(d.box, next[size_t(d.cascade)].box());
    std::vector<bool> columnsChanged(next.size(), first);
    for (size_t i = 0; i < next.size(); ++i) {
        if (first) {
            dirty.push_back({int(i), next[i].box(), true});
            continue;
        }
        if (next[i] == cascades_[i])
            continue;
        const std::vector<CellBox> regions = sdfgiScrollRegions(cascades_[i], next[i]);
        if (regions.size() == 1 && regions[0] == next[i].box())
            log::info(std::format("SDFGI: cascade {} rebuilt in full (moved past its extent)", i));
        for (const CellBox& box : regions)
            dirty.push_back({int(i), box, true});
        columnsChanged[i] = columnsChanged[i] || next[i].origin[0] != cascades_[i].origin[0] ||
                            next[i].origin[2] != cascades_[i].origin[2];
    }
    if (first)
        log::info(std::format("SDFGI: building {} cascades of {}x{}x{} cells ({} m to {} m)", next.size(), next[0].dims[0],
                              next[0].dims[1], next[0].dims[2], next[0].cellSize * float(next[0].dims[0]),
                              next.back().cellSize * float(next.back().dims[0])));

    // Static shapes and terrain tiles that came, went or moved: the cells near them, in every cascade.
    std::vector<SdfgiShape> shapes = frame.shapes;
    std::sort(shapes.begin(), shapes.end());
    std::vector<SdfgiTile> tiles = frame.tiles;
    std::sort(tiles.begin(), tiles.end());
    if (!first) {
        std::vector<SdfgiShape> changed;
        std::set_symmetric_difference(shapes.begin(), shapes.end(), shapesLast_.begin(), shapesLast_.end(),
                                      std::back_inserter(changed));
        for (const SdfgiShape& shape : changed)
            for (size_t i = 0; i < next.size(); ++i)
                if (const CellBox box = sdfgiCellsNear(next[i], shape.bounds, kSdfgiMaxDistance + 1.0f); !box.empty())
                    dirty.push_back({int(i), box, false});
        std::vector<SdfgiTile> tilesChanged;
        std::set_symmetric_difference(tiles.begin(), tiles.end(), tilesLast_.begin(), tilesLast_.end(),
                                      std::back_inserter(tilesChanged));
        for (const SdfgiTile& tile : tilesChanged) {
            const bool in = std::binary_search(tiles.begin(), tiles.end(), tile);
            if (sdfgiLogging())
                log::info(std::format("SDFGI: terrain tile {}_{} {}", tile.tx, tile.tz, in ? "streamed in" : "streamed out"));
            for (size_t i = 0; i < next.size(); ++i) {
                CellBox box = sdfgiCellsNear(next[i], {{tile.x0, 0.0f, tile.z0}, {tile.x1, 0.0f, tile.z1}}, 1.0f);
                box.min[1] = next[i].origin[1];
                box.max[1] = next[i].origin[1] + next[i].dims[1];
                box = intersect(box, next[i].box());
                if (!box.empty()) { // its probes keep their history: calmer than restarting them (sdfgi.html#terrain)
                    dirty.push_back({int(i), box, false});
                    columnsChanged[i] = true;
                }
            }
        }
    }
    shapesLast_ = std::move(shapes);
    tilesLast_ = std::move(tiles);
    cascades_ = next;

    const std::array<int, 3> dims = next[0].dims;
    float cascades[kMaxSdfgiCascades][4] = {};
    for (size_t i = 0; i < next.size(); ++i)
        cascades[i][0] = float(next[i].origin[0]), cascades[i][1] = float(next[i].origin[1]),
        cascades[i][2] = float(next[i].origin[2]), cascades[i][3] = next[i].cellSize;
    const float grid[4] = {float(dims[0]), float(dims[1]), float(dims[2]), float(next.size())};

    // Terrain columns of the cascades that scrolled in x / z or whose tiles changed.
    for (size_t i = 0; i < next.size(); ++i) {
        if (!columnsChanged[i])
            continue;
        const std::vector<std::array<float, 2>> columns = sdfgiTerrainColumns(next[i], frame.heightAt);
        const bgfx::Memory* mem = bgfx::alloc(uint32_t(columns.size() * 16));
        auto* out = reinterpret_cast<float*>(mem->data);
        for (const auto& [height, slope] : columns) {
            *out++ = height;
            *out++ = slope;
            *out++ = 0.0f;
            *out++ = 0.0f;
        }
        bgfx::updateTexture2D(terrain_, 0, 0, 0, uint16_t(i * size_t(dims[2])), uint16_t(dims[0]), uint16_t(dims[2]), mem);
    }

    // The shapes each box needs, packed back to back (6 vec4 each), up to kMaxSdfgiShapes this frame.
    struct Job {
        Dirty dirty;
        uint32_t first = 0, count = 0;
    };
    std::vector<Job> jobs;
    std::vector<float> packed;
    std::vector<size_t> rebuilt(next.size(), 0); // cells per cascade, for --log-sdfgi
    for (const Dirty& d : dirty) {
        if (d.box.empty())
            continue;
        const SdfgiCascade& c = next[size_t(d.cascade)];
        const float margin = kSdfgiMaxDistance * c.cellSize;
        const Aabb box{{float(d.box.min[0]) * c.cellSize - margin, float(d.box.min[1]) * c.cellSize - margin,
                        float(d.box.min[2]) * c.cellSize - margin},
                       {float(d.box.max[0]) * c.cellSize + margin, float(d.box.max[1]) * c.cellSize + margin,
                        float(d.box.max[2]) * c.cellSize + margin}};
        Job job{d, uint32_t(packed.size() / 24), 0};
        for (const SdfgiShape& shape : shapesLast_) {
            if (!overlaps(shape.bounds, box))
                continue;
            ++job.count;
            for (const auto& row : shape.rows)
                packed.insert(packed.end(), row.begin(), row.end());
            for (const float v : {shape.half.x, shape.half.y, shape.half.z, float(shape.type), shape.albedo.x,
                                  shape.albedo.y, shape.albedo.z, 0.0f, shape.emission.x, shape.emission.y,
                                  shape.emission.z, 0.0f})
                packed.push_back(v);
        }
        if (job.first + job.count > kMaxSdfgiShapes && !jobs.empty()) {
            packed.resize(size_t(job.first) * 24);
            pending_.push_back(d); // next frame
            continue;
        }
        rebuilt[size_t(d.cascade)] += d.box.cells();
        jobs.push_back(job);
    }
    if (!packed.empty())
        bgfx::update(shapes_, 0, bgfx::copy(packed.data(), uint32_t(packed.size() * sizeof(float))));

    bgfx::setViewName(view, "SDFGI");
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    for (const Job& job : jobs) {
        const CellBox& b = job.dirty.box;
        const float build[4][4] = {
            {float(b.min[0]), float(b.min[1]), float(b.min[2]), float(job.dirty.cascade)},
            {float(b.max[0] - b.min[0]), float(b.max[1] - b.min[1]), float(b.max[2] - b.min[2]), float(job.count)},
            {float(job.first), job.dirty.resetProbes ? 1.0f : 0.0f, frame.heightAt ? 1.0f : 0.0f, 0.0f},
            {frame.terrainAlbedo.x, frame.terrainAlbedo.y, frame.terrainAlbedo.z, 0.0f}};
        bgfx::setUniform(cascadesUniform_, cascades, kMaxSdfgiCascades);
        bgfx::setUniform(gridUniform_, grid);
        bgfx::setUniform(buildUniform_, build, 4);
        bgfx::setBuffer(0, shapes_, bgfx::Access::Read);
        bgfx::setImage(1, terrain_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA32F);
        bgfx::setImage(2, albedo_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA8);
        bgfx::setImage(3, sdf_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
        bgfx::setImage(4, probes_[current_], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
        bgfx::dispatch(view, build_, uint32_t(b.max[0] - b.min[0] + 3) / 4, uint32_t(b.max[1] - b.min[1] + 3) / 4,
                       uint32_t(b.max[2] - b.min[2] + 3) / 4);
    }

    // Light every surface cell, then march the probes' rays (into the other probe texture).
    const VoxelGiLights lights = packVoxelGiLights(frame.sunDirection, frame.sunColor, false, frame.lights,
                                                   std::vector<float>(frame.lights.size(), -1.0f),
                                                   {next.back().worldMin(), next.back().worldMax()});
    const float params[2][4] = {{float(preset.rays), float(preset.steps), preset.hysteresis, float(frame_)},
                                {s.bounceFeedback, float(lights.count), 0.0f, 0.0f}};
    bgfx::setUniform(cascadesUniform_, cascades, kMaxSdfgiCascades);
    bgfx::setUniform(gridUniform_, grid);
    bgfx::setUniform(paramsUniform_, params, 2);
    bgfx::setUniform(lightsUniform_, lights.data.data(), uint16_t(kMaxVoxelGiLights * 4));
    bgfx::setImage(0, sdf_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(1, albedo_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA8);
    bgfx::setImage(2, probes_[current_], 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(3, lightVolume_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    bgfx::dispatch(view, light_, uint32_t(dims[0] / 4), uint32_t(dims[1] / 4),
                   uint32_t(dims[2] / 4) * uint32_t(next.size()));

    float sky[9][4] = {};
    if (frame.sky) {
        for (size_t i = 0; i < 9; ++i)
            std::memcpy(sky[i], &frame.skySh[i * 3], 3 * sizeof(float));
    } else {
        std::memcpy(sky[0], &frame.ambient, sizeof(bx::Vec3));
    }
    // This frame's turn of the ray set: a deterministic walk (golden-ratio angles) so runs repeat.
    const float angle = float(frame_) * 2.39996323f;
    const bx::Vec3 axis = bx::normalize(bx::Vec3{std::sin(float(frame_) * 1.7f), std::cos(float(frame_) * 2.3f),
                                                 std::sin(float(frame_) * 0.7f + 1.0f)});
    const bx::Quaternion turn = bx::fromAxisAngle(axis, angle);
    float rotation[16];
    bx::mtxFromQuaternion(rotation, turn);
    const float rows[3][4] = {{rotation[0], rotation[4], rotation[8], 0.0f},
                              {rotation[1], rotation[5], rotation[9], 0.0f},
                              {rotation[2], rotation[6], rotation[10], 0.0f}};
    bgfx::setUniform(cascadesUniform_, cascades, kMaxSdfgiCascades);
    bgfx::setUniform(gridUniform_, grid);
    bgfx::setUniform(paramsUniform_, params, 2);
    bgfx::setUniform(skyUniform_, sky, 9);
    bgfx::setUniform(rotationUniform_, rows, 3);
    bgfx::setImage(0, sdf_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(1, lightVolume_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(2, probes_[current_], 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(3, probes_[1 - current_], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    const std::array<int, 3> probeDims = next[0].probeDims();
    const auto probes = uint32_t(probeDims[0] * probeDims[1] * probeDims[2]) * uint32_t(next.size());
    bgfx::dispatch(view, probe_, (probes + 63) / 64);
    current_ = 1 - current_;

    if (sdfgiLogging()) {
        std::string counts;
        for (size_t i = 0; i < rebuilt.size(); ++i)
            counts += std::format("{}c{} {}", i ? ", " : "", i, rebuilt[i]);
        log::info(std::format("SDFGI frame {}: rebuilt cells {}; {} probes updated{}", frame_, counts, probes,
                              pending_.empty() ? "" : std::format(" ({} regions wait)", pending_.size())));
    }
    ++frame_;
}

void SdfgiRenderer::bind(bool on, bool grid, bool specular) const
{
    float params[6][4] = {};
    if (on && ready()) {
        for (size_t i = 0; i < cascades_.size(); ++i) {
            const SdfgiCascade& c = cascades_[i];
            params[i][0] = float(c.origin[0] / kSdfgiProbeCells);
            params[i][1] = float(c.origin[1] / kSdfgiProbeCells);
            params[i][2] = float(c.origin[2] / kSdfgiProbeCells);
            params[i][3] = c.probeSpacing();
        }
        const std::array<int, 3> probes = cascades_[0].probeDims();
        const float tail[2][4] = {{float(probes[0]), float(probes[1]), float(probes[2]), float(cascades_.size())},
                                  {energy_, grid ? 1.0f : 0.0f, specular ? 1.0f : 0.0f, 1.0f}};
        std::memcpy(params[4], tail, sizeof(tail));
        bgfx::setTexture(10, meshSampler_, probes_[current_]);
    }
    bgfx::setUniform(meshUniform_, params, 6);
}

} // namespace kast
