#include "lightmap_bake.h"

#include "animation.h"
#include "files.h"
#include "log.h"
#include "material.h"
#include "project.h"
#include "sky.h"
#include "terrain.h"

#include <xatlas.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <thread>

namespace kast {
namespace {

constexpr float kRayOffset = 2e-3f;      // meters a ray starts off its surface, against hitting it again
constexpr float kRayMax = 1e6f;          // meters: "to infinity"
constexpr uint32_t kPadding = 2;         // texels xatlas leaves around each chart, so bilinear filtering stays inside
constexpr int kRectGap = 2;              // texels between two meshes' rects in a page
constexpr int kDilatePasses = 8;         // seam dilation: rings of texels filled around (and inside) the charts
constexpr float kTexelBackfaces = 0.25f; // a texel whose first hits are more back faces than this is inside geometry
constexpr float kProbeBackfaces = 0.1f;  // the same for a probe of the grid
constexpr int kMinProbeRays = 64;
constexpr size_t kChunk = 64;            // texels (or probes) a worker takes at a time

float luminance(const bx::Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// A frame around unit n (Duff et al. 2017, branchless).
void frame(const bx::Vec3& n, bx::Vec3& t, bx::Vec3& b)
{
    const float sign = std::copysign(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float c = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x};
    b = {c, sign + n.y * n.y * a, -n.y};
}

// The van der Corput sequence in base 2: the Hammersley set's second coordinate.
float radicalInverse(uint32_t i)
{
    i = (i << 16) | (i >> 16);
    i = ((i & 0x55555555u) << 1) | ((i & 0xAAAAAAAAu) >> 1);
    i = ((i & 0x33333333u) << 2) | ((i & 0xCCCCCCCCu) >> 2);
    i = ((i & 0x0F0F0F0Fu) << 4) | ((i & 0xF0F0F0F0u) >> 4);
    i = ((i & 0x00FF00FFu) << 8) | ((i & 0xFF00FF00u) >> 8);
    return float(i) * 2.3283064365386963e-10f;
}

// Sample i of count over the unit square: Hammersley, shifted by (du, dv) (Cranley-Patterson, per texel), so every
// texel's samples are well spread but no two texels share them.
std::pair<float, float> hammersley(uint32_t i, uint32_t count, float du, float dv)
{
    float u = (float(i) + 0.5f) / float(count) + du;
    float v = radicalInverse(i) + dv;
    return {u - std::floor(u), v - std::floor(v)};
}

// Cosine-weighted direction around unit n (pdf cos / pi).
bx::Vec3 cosineDirection(const bx::Vec3& n, float u, float v)
{
    bx::Vec3 t{0.0f, 0.0f, 0.0f}, b{0.0f, 0.0f, 0.0f};
    frame(n, t, b);
    const float r = std::sqrt(u), phi = 2.0f * bx::kPi * v;
    const float z = std::sqrt(std::max(0.0f, 1.0f - u));
    return bx::normalize(bx::add(bx::add(bx::mul(t, r * std::cos(phi)), bx::mul(b, r * std::sin(phi))), bx::mul(n, z)));
}

// Uniform direction on the sphere.
bx::Vec3 sphereDirection(float u, float v)
{
    const float z = 1.0f - 2.0f * u, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * bx::kPi * v;
    return {r * std::cos(phi), z, r * std::sin(phi)};
}

bx::Vec3 transformPoint(const bx::Vec3& p, const Mat4& m) { return bx::mul(p, m.data()); }

} // namespace

bx::Vec3 transformNormal(const bx::Vec3& n, const Mat4& m)
{
    const bx::Vec3 c0{m[0], m[1], m[2]}, c1{m[4], m[5], m[6]}, c2{m[8], m[9], m[10]};
    const bx::Vec3 r = bx::add(bx::add(bx::mul(bx::cross(c1, c2), n.x), bx::mul(bx::cross(c2, c0), n.y)),
                               bx::mul(bx::cross(c0, c1), n.z));
    const float det = bx::dot(c0, bx::cross(c1, c2));
    const bx::Vec3 out = det < 0.0f ? bx::neg(r) : r;
    const float length = bx::length(out);
    return length > 0.0f ? bx::mul(out, 1.0f / length) : bx::Vec3{0.0f, 1.0f, 0.0f};
}

namespace {

// The scale of each of m's axes (no shear assumed).
bx::Vec3 axisScale(const Mat4& m)
{
    return {bx::length({m[0], m[1], m[2]}), bx::length({m[4], m[5], m[6]}), bx::length({m[8], m[9], m[10]})};
}

std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string s = path.generic_u8string();
    return {s.begin(), s.end()};
}

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// The average linear color of a PNG / JPG texture (a file, or "<model>#<i>"), white if it can't be read.
// ponytail: KTX / DDS textures count as white; decode them with bimg if baked colors from them look off.
bx::Vec3 averageTexture(const std::filesystem::path& root, const std::string& path)
{
    if (path.empty())
        return {1.0f, 1.0f, 1.0f};
    std::string bytes;
    if (const auto embedded = models().embeddedTexture(path)) {
        bytes = std::string(*embedded);
    } else {
        std::string ignored;
        const auto file = resolveProjectPath(root, path, ignored);
        if (!file || !readFile(*file, bytes))
            return {1.0f, 1.0f, 1.0f};
    }
    const std::optional<MipImage> image = isGpuImage(bytes, path) ? std::nullopt : decodeImage(bytes, true);
    if (!image || image->rgba.size() < 4)
        return {1.0f, 1.0f, 1.0f};
    const uint8_t* last = &image->rgba[image->rgba.size() - 4]; // the 1 x 1 mip: the average (filtered in linear)
    return {srgbToLinear(last[0] / 255.0f), srgbToLinear(last[1] / 255.0f), srgbToLinear(last[2] / 255.0f)};
}

BakeSurface surfaceOf(const Material& material, const std::filesystem::path& root)
{
    BakeSurface surface;
    const bx::Vec3 base{material.baseColor[0], material.baseColor[1], material.baseColor[2]};
    surface.albedo = bx::mul(bx::mul(base, averageTexture(root, material.baseColorTexture)), 1.0f - material.metallic);
    surface.emission = bx::mul(emissiveRadiance(material), averageTexture(root, material.emissiveTexture));
    surface.twoSided = material.cullMode == CullMode::None;
    return surface;
}

// Surfaces by key ("file:<.mat>", "model:<path>#<slot>"), each made once.
class Surfaces {
public:
    Surfaces(const std::filesystem::path& root, LightmapBakeInput& input) : root_(root), input_(input) {}
    uint32_t file(const std::string& path)
    {
        return add("file:" + path, [&] {
            if (path.empty())
                return surfaceOf(defaultMaterial(), root_);
            Material material;
            std::string error, ignored;
            const auto resolved = resolveProjectPath(root_, path, ignored);
            if (!resolved || !loadMaterial(*resolved, material, error)) {
                input_.warnings.push_back(std::format("material '{}' doesn't load; baked as the default grey", path));
                return surfaceOf(defaultMaterial(), root_);
            }
            return surfaceOf(material, root_);
        });
    }
    uint32_t embedded(const std::string& model, uint32_t slot, const Material& material)
    {
        return add(std::format("model:{}#{}", model, slot), [&] { return surfaceOf(material, root_); });
    }

private:
    template <typename Make>
    uint32_t add(const std::string& key, Make make)
    {
        const auto [it, added] = index_.try_emplace(key, static_cast<uint32_t>(input_.surfaces.size()));
        if (added)
            input_.surfaces.push_back(make());
        return it->second;
    }
    const std::filesystem::path& root_;
    LightmapBakeInput& input_;
    std::map<std::string, uint32_t> index_;
};

const char* primitiveName(Primitive primitive)
{
    return primitive == Primitive::Cube ? "cube" : primitive == Primitive::Plane ? "plane" : "sphere";
}

BakeMesh primitiveBakeMesh(Primitive primitive, uint32_t surface)
{
    const MeshData data = primitiveMesh(primitive);
    BakeMesh mesh;
    for (const MeshVertex& v : data.vertices) {
        mesh.positions.push_back({v.x, v.y, v.z});
        mesh.normals.push_back({v.nx, v.ny, v.nz});
    }
    mesh.indices.assign(data.indices.begin(), data.indices.end());
    mesh.surface = surface;
    return mesh;
}

// The 8 x 8 bytes key of an unwrap: entities of one source at one scale and density share it (and its rect size).
std::string unwrapKey(const BakeEntity& entity)
{
    const bx::Vec3 s = axisScale(entity.world);
    return std::format("{}@{:.4f},{:.4f},{:.4f}@{:.4f}", entity.source, s.x, s.y, s.z, entity.density);
}

// A point to cast a shadow ray at for light from p: the light's position, a random point on an area light's rect,
// or far along a directional light. Returns the distance (kRayMax for the sun) and sets dir (unit).
float shadowTarget(const BakeLight& light, const bx::Vec3& p, BakeRng& rng, bx::Vec3& dir)
{
    const LocalLightSample& s = light.sample;
    if (light.directional) {
        dir = bx::neg(s.direction);
        return kRayMax;
    }
    bx::Vec3 target = s.position;
    if (s.type == LocalLightType::Area)
        target = bx::add(target, bx::add(bx::mul(s.halfWidth, rng.uniform() * 2.0f - 1.0f),
                                         bx::mul(s.halfHeight, rng.uniform() * 2.0f - 1.0f)));
    const bx::Vec3 to = bx::sub(target, p);
    const float d = bx::length(to);
    dir = d > 0.0f ? bx::mul(to, 1.0f / d) : bx::Vec3{0.0f, 1.0f, 0.0f};
    return d;
}

// Places w x h rects (in order) into pages of at most kLightmapAtlasSize, shelf by shelf; returns each rect's page and
// corner, and the pages' sizes (rounded up to 4).
struct Placement {
    uint32_t page = 0;
    int x = 0, y = 0;
};
std::vector<Placement> packRects(const std::vector<std::pair<int, int>>& sizes, std::vector<std::pair<int, int>>& pages)
{
    std::vector<size_t> order(sizes.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::ranges::stable_sort(order, [&](size_t a, size_t b) {
        return sizes[a].second != sizes[b].second ? sizes[a].second > sizes[b].second : sizes[a].first > sizes[b].first;
    });
    std::vector<Placement> placed(sizes.size());
    int x = 0, y = 0, shelf = 0;
    pages.clear();
    for (size_t i : order) {
        const auto [w, h] = sizes[i];
        if (pages.empty())
            pages.push_back({0, 0});
        if (x + w > kLightmapAtlasSize) { // next shelf
            x = 0;
            y += shelf + kRectGap;
            shelf = 0;
        }
        if (y + h > kLightmapAtlasSize) { // next page
            pages.push_back({0, 0});
            x = y = shelf = 0;
        }
        placed[i] = {static_cast<uint32_t>(pages.size() - 1), x, y};
        auto& [pw, ph] = pages.back();
        pw = std::max(pw, x + w);
        ph = std::max(ph, y + h);
        x += w + kRectGap;
        shelf = std::max(shelf, h);
    }
    for (auto& [w, h] : pages) {
        w = std::min<int>((w + 3) / 4 * 4, kLightmapAtlasSize);
        h = std::min<int>((h + 3) / 4 * 4, kLightmapAtlasSize);
    }
    return placed;
}

// A texel to bake: where it is and which way it faces (world), and where its result goes.
struct Texel {
    bx::Vec3 position{0.0f, 0.0f, 0.0f}, normal{0.0f, 1.0f, 0.0f};
    uint32_t page = 0; // atlas pages first, then terrain tiles
    uint32_t index = 0; // y * width + x in its page
};

// One image being baked: an atlas page or a terrain tile.
struct Image {
    int width = 0, height = 0;
    std::vector<float> irradiance; // rgba
    std::vector<float> direction;  // xyz w
    std::vector<uint8_t> state;    // 0 empty, 1 baked, 2 inside geometry (dilated over)
};

// Fills empty and inside-geometry texels from their baked neighbours, ring by ring.
void dilate(Image& image)
{
    for (int pass = 0; pass < kDilatePasses; ++pass) {
        std::vector<uint8_t> next = image.state;
        bool changed = false;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                const size_t i = size_t(y) * image.width + x;
                if (image.state[i] == 1)
                    continue;
                float sum[8] = {};
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= image.width || ny >= image.height)
                            continue;
                        const size_t j = size_t(ny) * image.width + nx;
                        if (image.state[j] != 1)
                            continue;
                        for (int c = 0; c < 4; ++c) {
                            sum[c] += image.irradiance[j * 4 + c];
                            sum[4 + c] += image.direction[j * 4 + c];
                        }
                        ++count;
                    }
                if (count == 0)
                    continue;
                for (int c = 0; c < 4; ++c) {
                    image.irradiance[i * 4 + c] = sum[c] / float(count);
                    image.direction[i * 4 + c] = sum[4 + c] / float(count);
                }
                next[i] = 1;
                changed = true;
            }
        image.state = std::move(next);
        if (!changed)
            break;
    }
}

// Runs work(i) for i in 0..count on every hardware thread, chunk by chunk, until done or cancelled; done counts up.
template <typename Work>
void parallelFor(size_t count, LightmapBakeProgress& progress, std::atomic<size_t>& done, Work work)
{
    std::atomic<size_t> next{0};
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            while (!progress.cancel.load(std::memory_order_relaxed)) {
                const size_t first = next.fetch_add(kChunk);
                if (first >= count)
                    return;
                const size_t last = std::min(count, first + kChunk);
                for (size_t i = first; i < last; ++i)
                    work(i);
                done.fetch_add(last - first);
            }
        });
}

} // namespace

bx::Vec3 BakeSky::radiance(const bx::Vec3& direction) const
{
    switch (kind) {
    case Kind::Procedural:
        return bx::mul(proceduralSkyColor(sky, direction.y), tint);
    case Kind::Cube:
        return bx::mul(sampleCubeSides(cube, direction), tint);
    default:
        return tint;
    }
}

BakeRng::BakeRng(uint64_t seed, uint64_t stream) : inc_((stream << 1u) | 1u)
{
    next();
    state_ += seed;
    next();
}

uint32_t BakeRng::next()
{
    const uint64_t old = state_;
    state_ = old * 6364136223846793005ull + inc_;
    const auto xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

bool gatherLightmapBake(const Scene& scene, const std::filesystem::path& root, const std::string& scenePath,
                        LightmapBakeInput& out, std::string& error)
{
    LightmapBakeInput input;
    input.settings = scene.lighting.lightmaps;
    input.hash = lightmapSceneHash(scene);
    const std::string sidecar = lightmapSidecarPath(scenePath);
    input.folder = (root / std::filesystem::path(reinterpret_cast<const char8_t*>(sidecar.c_str()))).parent_path();
    Surfaces surfaces(root, input);
    const entt::registry& registry = scene.registry();
    const SceneLightmaps& settings = scene.lighting.lightmaps;

    for (auto [entity, mesh] : registry.view<MeshRenderer>().each()) {
        if (mesh.giMode != GiMode::Static)
            continue;
        BakeEntity e{.id = registry.get<PersistentId>(entity).value, .name = registry.get<Name>(entity).value,
                     .source = std::format("primitive:{}", primitiveName(mesh.primitive)),
                     .density = settings.texelsPerMeter * mesh.lightmapScale};
        scene.worldMatrix(entity, e.world.data());
        e.meshes.push_back(primitiveBakeMesh(mesh.primitive, surfaces.file(mesh.material)));
        input.entities.push_back(std::move(e));
    }
    for (auto [entity, renderer] : registry.view<ModelRenderer>().each()) {
        if (renderer.giMode != GiMode::Static)
            continue;
        const std::string& name = registry.get<Name>(entity).value;
        const std::shared_ptr<const Model> model = models().get(renderer.model);
        if (!model) {
            input.warnings.push_back(std::format("'{}': model '{}' doesn't load; left out of the bake", name, renderer.model));
            continue;
        }
        if (std::ranges::any_of(model->meshes, [](const ModelMesh& m) { return !m.joints.empty(); })) {
            input.warnings.push_back(std::format("'{}': a skinned model can't be Static; lit as Dynamic", name));
            continue;
        }
        BakeEntity e{.id = registry.get<PersistentId>(entity).value, .name = name, .source = "model:" + renderer.model,
                     .density = settings.texelsPerMeter * renderer.lightmapScale};
        scene.worldMatrix(entity, e.world.data());
        std::vector<Mat4> globals;
        nodeGlobals(*model, bindPose(*model), globals);
        for (size_t i = 0; i < model->meshes.size(); ++i) {
            const ModelMesh& source = model->meshes[i];
            const Mat4& node = globals[static_cast<size_t>(source.node)];
            BakeMesh mesh;
            for (const ModelVertex& v : source.vertices) {
                mesh.positions.push_back(transformPoint({v.x, v.y, v.z}, node));
                mesh.normals.push_back(transformNormal({v.nx, v.ny, v.nz}, node));
            }
            mesh.indices = source.indices;
            if (!source.uv2.empty() && std::ranges::all_of(model->meshes, [](const ModelMesh& m) { return !m.uv2.empty(); }))
                mesh.uv2 = source.uv2;
            const std::string* slot =
                source.material < renderer.materials.size() ? &renderer.materials[source.material] : nullptr;
            mesh.surface = slot && !slot->empty()
                               ? surfaces.file(*slot)
                               : surfaces.embedded(renderer.model, source.material,
                                                   source.material < model->materials.size()
                                                       ? model->materials[source.material].material
                                                       : defaultMaterial());
            e.meshes.push_back(std::move(mesh));
        }
        input.entities.push_back(std::move(e));
    }
    std::ranges::sort(input.entities, {}, &BakeEntity::id); // the bake doesn't depend on the registry's order

    const auto worldOf = [&](entt::entity entity) {
        float m[16];
        scene.worldMatrix(entity, m);
        return std::array<bx::Vec3, 4>{bx::Vec3{m[12], m[13], m[14]}, bx::normalize(transformDir({0.0f, 0.0f, -1.0f}, m)),
                                       bx::normalize(transformDir({1.0f, 0.0f, 0.0f}, m)),
                                       bx::normalize(transformDir({0.0f, 1.0f, 0.0f}, m))};
    };
    if (const entt::entity sun = firstDepthFirst<DirectionalLight>(scene); sun != entt::null) {
        const DirectionalLight& light = registry.get<DirectionalLight>(sun);
        if (light.bakeMode != LightBakeMode::Realtime)
            input.lights.push_back({.sample = {.color = bx::mul(light.color, light.intensity), .direction = worldOf(sun)[1]},
                                    .directional = true,
                                    .full = light.bakeMode == LightBakeMode::Full});
    }
    for (auto [entity, light] : registry.view<PointLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime)
            input.lights.push_back({.sample = {.position = worldOf(entity)[0], .range = light.range,
                                               .color = bx::mul(light.color, light.intensity)},
                                    .full = light.bakeMode == LightBakeMode::Full});
    for (auto [entity, light] : registry.view<SpotLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime) {
            const auto w = worldOf(entity);
            input.lights.push_back({.sample = {.position = w[0], .range = light.range,
                                               .color = bx::mul(light.color, light.intensity),
                                               .type = LocalLightType::Spot, .direction = w[1],
                                               .cosInner = std::cos(bx::toRad(light.innerAngle)),
                                               .cosOuter = std::cos(bx::toRad(light.outerAngle))},
                                    .full = light.bakeMode == LightBakeMode::Full});
        }
    for (auto [entity, light] : registry.view<AreaLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime) {
            const auto w = worldOf(entity);
            input.lights.push_back({.sample = {.position = w[0], .range = light.range,
                                               .color = bx::mul(light.color, light.intensity),
                                               .type = LocalLightType::Area, .direction = w[1],
                                               .halfWidth = bx::mul(w[2], 0.5f * light.width),
                                               .halfHeight = bx::mul(w[3], 0.5f * light.height),
                                               .twoSided = light.twoSided},
                                    .full = light.bakeMode == LightBakeMode::Full});
        }

    // The environment: as Renderer::render lights the ambient (sky times the ambient color, or the flat ambient).
    const SceneLighting& lighting = scene.lighting;
    input.sky.tint = bx::mul(lighting.ambientColor, lighting.ambientIntensity);
    input.sky.sky = scene.sky;
    input.sky.kind = scene.sky.mode == SkyMode::Off ? BakeSky::Kind::Uniform : BakeSky::Kind::Procedural;
    if (scene.sky.mode == SkyMode::Cubemap) { // a cube that can't be read falls back to the gradient, as the renderer's
        std::string bytes, ignored;
        if (!scene.sky.cube.empty()) {
            const auto file = resolveProjectPath(root, scene.sky.cube, ignored);
            if (file && readFile(*file, bytes))
                if (std::optional<CubeSides> cube = cubeFileSides(bytes, true)) {
                    input.sky.cube = std::move(*cube);
                    input.sky.kind = BakeSky::Kind::Cube;
                }
        } else {
            std::array<MipImage, 6> faces;
            bool decoded = true;
            for (size_t i = 0; i < 6 && decoded; ++i) {
                const auto file = resolveProjectPath(root, scene.sky.faces[i], ignored);
                std::optional<MipImage> image;
                decoded = file && readFile(*file, bytes) && (image = decodeImage(bytes, true));
                if (decoded)
                    faces[i] = std::move(*image);
            }
            if (decoded && skyFacesError(faces).empty()) {
                input.sky.cube = skyFacesSides(faces);
                input.sky.kind = BakeSky::Kind::Cube;
            }
        }
        if (input.sky.kind != BakeSky::Kind::Cube)
            input.warnings.push_back("the cubemap sky doesn't load; baked with the procedural sky");
    }

    // Lightmapped terrain: every declared tile of its map.
    TerrainStreamer streamer;
    bool started = false;
    for (auto [entity, terrain] : registry.view<Terrain>().each()) {
        if (!terrain.lightmap)
            continue;
        if (!std::exchange(started, true))
            streamer.start(root);
        const auto id = static_cast<uint16_t>(terrain.map);
        const map::MapInfo* info = streamer.map(id);
        if (!info) {
            input.warnings.push_back(std::format("terrain: no map with id {}; left out of the bake", terrain.map));
            continue;
        }
        BakeTerrain baked{.folder = *streamer.folder(id) / "lightmaps", .size = map::kTileSamples,
                          .surface = surfaces.file(info->material)};
        std::set<std::pair<int, int>> seen;
        for (const map::TileRect& rect : info->tiles)
            for (int tz = rect.z0; tz <= rect.z1; ++tz)
                for (int tx = rect.x0; tx <= rect.x1; ++tx) {
                    if (!seen.insert({tx, tz}).second)
                        continue;
                    const auto data = streamer.loadTile(id, {tx, tz}, false);
                    BakeTerrainTile tile{.tx = tx, .tz = tz};
                    const int n = map::kTileSamples;
                    for (int v = 0; v < n; ++v)
                        for (int u = 0; u < n; ++u) {
                            const float y = map::sample_height(data->heights[size_t(v) * n + size_t(u)], *info);
                            const auto normal = map::normal_at(data->heights, *info, u, v);
                            tile.positions.push_back({float(tx) * map::kTileSize + float(u), y,
                                                      float(tz) * map::kTileSize + float(v)});
                            tile.normals.push_back({normal[0], normal[1], normal[2]});
                        }
                    baked.tiles.push_back(std::move(tile));
                }
        input.terrain.push_back(std::move(baked));
    }
    if (input.entities.empty() && input.terrain.empty()) {
        error = "nothing to bake: no Static mesh and no Terrain with Bake Lightmap";
        return false;
    }
    out = std::move(input);
    return true;
}

bool unwrapLightmapMeshes(const std::vector<BakeMesh>& meshes, const bx::Vec3& scale, float density, LightmapUnwrap& out,
                          std::string& error)
{
    LightmapUnwrap result;
    const bool given = !meshes.empty() && std::ranges::all_of(meshes, [](const BakeMesh& m) { return !m.uv2.empty(); });
    if (given) { // the source's own second UV set: kept, in a square rect sized by the scaled surface area
        double area = 0.0;
        for (const BakeMesh& mesh : meshes) {
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const bx::Vec3 a = bx::mul(mesh.positions[mesh.indices[i]], scale);
                const bx::Vec3 b = bx::mul(mesh.positions[mesh.indices[i + 1]], scale);
                const bx::Vec3 c = bx::mul(mesh.positions[mesh.indices[i + 2]], scale);
                area += 0.5 * bx::length(bx::cross(bx::sub(b, a), bx::sub(c, a)));
            }
            LightmapSubmesh sub{.sourceVertices = static_cast<uint32_t>(mesh.positions.size()), .uv = mesh.uv2,
                                .indices = mesh.indices};
            for (uint32_t i = 0; i < sub.sourceVertices; ++i)
                sub.xref.push_back(i);
            result.submeshes.push_back(std::move(sub));
        }
        // ponytail: assumes the authored layout uses about two thirds of its square.
        const int side = std::clamp(int(std::ceil(std::sqrt(area) * density * 1.25)), 8, int(kLightmapAtlasSize));
        result.width = result.height = static_cast<uint16_t>(side);
        out = std::move(result);
        return true;
    }
    for (int attempt = 0; attempt < 6; ++attempt) {
        xatlas::Atlas* atlas = xatlas::Create();
        std::vector<std::vector<bx::Vec3>> scaled(meshes.size());
        for (size_t m = 0; m < meshes.size(); ++m) {
            for (const bx::Vec3& p : meshes[m].positions)
                scaled[m].push_back(bx::mul(p, scale));
            xatlas::MeshDecl decl;
            decl.vertexPositionData = scaled[m].data();
            decl.vertexPositionStride = sizeof(bx::Vec3);
            decl.vertexNormalData = meshes[m].normals.data();
            decl.vertexNormalStride = sizeof(bx::Vec3);
            decl.vertexCount = static_cast<uint32_t>(scaled[m].size());
            decl.indexData = meshes[m].indices.data();
            decl.indexCount = static_cast<uint32_t>(meshes[m].indices.size());
            decl.indexFormat = xatlas::IndexFormat::UInt32;
            if (const xatlas::AddMeshError e = xatlas::AddMesh(atlas, decl, static_cast<uint32_t>(meshes.size()));
                e != xatlas::AddMeshError::Success) {
                xatlas::Destroy(atlas);
                error = std::format("xatlas: {}", xatlas::StringForEnum(e));
                return false;
            }
        }
        xatlas::PackOptions pack;
        pack.padding = kPadding;
        pack.texelsPerUnit = density;
        pack.bilinear = true;
        xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
        const uint32_t width = atlas->width, height = atlas->height;
        if (width == 0 || height == 0) {
            xatlas::Destroy(atlas);
            error = "xatlas made no charts";
            return false;
        }
        if (width > kLightmapAtlasSize || height > kLightmapAtlasSize) { // too big for a page: fewer texels per meter
            xatlas::Destroy(atlas);
            density *= 0.9f * float(kLightmapAtlasSize) / float(std::max(width, height));
            continue;
        }
        result.width = static_cast<uint16_t>(width);
        result.height = static_cast<uint16_t>(height);
        for (uint32_t m = 0; m < atlas->meshCount; ++m) {
            const xatlas::Mesh& mesh = atlas->meshes[m];
            LightmapSubmesh sub{.sourceVertices = static_cast<uint32_t>(meshes[m].positions.size())};
            for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
                sub.xref.push_back(mesh.vertexArray[v].xref);
                sub.uv.push_back(mesh.vertexArray[v].uv[0] / float(width));
                sub.uv.push_back(mesh.vertexArray[v].uv[1] / float(height));
            }
            sub.indices.assign(mesh.indexArray, mesh.indexArray + mesh.indexCount);
            result.submeshes.push_back(std::move(sub));
        }
        xatlas::Destroy(atlas);
        out = std::move(result);
        return true;
    }
    error = "the mesh doesn't fit a lightmap page";
    return false;
}

bx::Vec3 bakedLightIrradiance(const BakeLight& light, const bx::Vec3& p, const bx::Vec3& n)
{
    const LocalLightSample& s = light.sample;
    if (light.directional)
        return bx::mul(s.color, std::max(bx::dot(n, bx::neg(s.direction)), 0.0f));
    const bx::Vec3 to = bx::sub(s.position, p);
    const float d = bx::length(to);
    if (s.type == LocalLightType::Area) {
        // fs_mesh.sc areaShade's diffuse: the rect's form factor times its range window (areaVisibility).
        const float ratio = d / s.range;
        const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
        const bx::Vec3 c = to;
        const std::array<bx::Vec3, 4> corners = {bx::sub(bx::sub(c, s.halfWidth), s.halfHeight),
                                                 bx::sub(bx::add(c, s.halfWidth), s.halfHeight),
                                                 bx::add(bx::add(c, s.halfWidth), s.halfHeight),
                                                 bx::add(bx::sub(c, s.halfWidth), s.halfHeight)};
        return bx::mul(s.color, rectFormFactor(corners, n, s.twoSided) * window * window);
    }
    const bx::Vec3 l = d > 0.0f ? bx::mul(to, 1.0f / d) : bx::Vec3{0.0f, 1.0f, 0.0f};
    float attenuation = pointLightFalloff(d, s.range);
    if (s.type == LocalLightType::Spot)
        attenuation *= spotConeFalloff(bx::dot(bx::neg(l), s.direction), s.cosInner, s.cosOuter);
    return bx::mul(s.color, attenuation * std::max(bx::dot(n, l), 0.0f));
}

LightmapTracer::LightmapTracer(const LightmapBakeInput& input) : input_(input)
{
    std::vector<BvhTriangle> triangles;
    for (const BakeEntity& entity : input.entities) {
        // Each submesh as drawn: model-space positions through the entity's world matrix.
        for (const BakeMesh& mesh : entity.meshes)
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                triangles.push_back({transformPoint(mesh.positions[mesh.indices[i]], entity.world),
                                     transformPoint(mesh.positions[mesh.indices[i + 1]], entity.world),
                                     transformPoint(mesh.positions[mesh.indices[i + 2]], entity.world),
                                     static_cast<uint32_t>(surfaceOf_.size())});
                surfaceOf_.push_back(mesh.surface);
            }
    }
    for (const BakeTerrain& terrain : input.terrain)
        for (const BakeTerrainTile& tile : terrain.tiles)
            map::for_each_triangle([&](uint32_t a, uint32_t b, uint32_t c) {
                triangles.push_back({tile.positions[a], tile.positions[b], tile.positions[c],
                                     static_cast<uint32_t>(surfaceOf_.size())});
                surfaceOf_.push_back(terrain.surface);
            });
    bvh_.build(std::move(triangles));
}

bx::Vec3 LightmapTracer::direct(const bx::Vec3& p, const bx::Vec3& n, bool fullOnly, BakeRng& rng, bx::Vec3* weighted) const
{
    bx::Vec3 sum{0.0f, 0.0f, 0.0f};
    const bx::Vec3 origin = bx::add(p, bx::mul(n, kRayOffset));
    for (const BakeLight& light : input_.lights) {
        if (fullOnly && !light.full)
            continue;
        const bx::Vec3 e = bakedLightIrradiance(light, p, n);
        if (e.x <= 0.0f && e.y <= 0.0f && e.z <= 0.0f)
            continue;
        bx::Vec3 dir{0.0f, 0.0f, 0.0f};
        const float distance = shadowTarget(light, origin, rng, dir);
        if (bvh_.occluded(origin, dir, 0.0f, distance - kRayOffset))
            continue;
        sum = bx::add(sum, e);
        if (weighted)
            *weighted = bx::add(*weighted, bx::mul(dir, luminance(e)));
    }
    return sum;
}

bx::Vec3 LightmapTracer::radiance(const bx::Vec3& origin, const bx::Vec3& direction, int bounces, BakeRng& rng,
                                  bool& backface) const
{
    backface = false;
    bx::Vec3 sum{0.0f, 0.0f, 0.0f}, throughput{1.0f, 1.0f, 1.0f};
    bx::Vec3 o = origin, d = direction;
    for (int depth = 1;; ++depth) {
        const std::optional<BvhHit> hit = bvh_.intersect(o, d, 0.0f, kRayMax);
        if (!hit)
            return bx::add(sum, bx::mul(throughput, input_.sky.radiance(d)));
        const BvhTriangle& t = bvh_.triangles()[hit->triangle];
        const BakeSurface& surface = input_.surfaces[surfaceOf_[t.id]];
        bx::Vec3 n = bx::normalize(bx::cross(bx::sub(t.b, t.a), bx::sub(t.c, t.a)));
        if (bx::dot(n, d) > 0.0f) {
            if (!surface.twoSided) { // the back of a one-sided surface: inside geometry, black
                backface = depth == 1;
                return sum;
            }
            n = bx::neg(n);
        }
        sum = bx::add(sum, bx::mul(throughput, surface.emission));
        if (depth > bounces)
            return sum;
        throughput = bx::mul(throughput, surface.albedo);
        if (throughput.x <= 0.0f && throughput.y <= 0.0f && throughput.z <= 0.0f)
            return sum;
        const bx::Vec3 p = bx::add(o, bx::mul(d, hit->t));
        sum = bx::add(sum, bx::mul(throughput, direct(p, n, false, rng, nullptr)));
        o = bx::add(p, bx::mul(n, kRayOffset));
        d = cosineDirection(n, rng.uniform(), rng.uniform());
    }
}

LightmapTracer::Gather LightmapTracer::gather(const bx::Vec3& p, const bx::Vec3& n, int samples, int bounces, bool directFull,
                                              BakeRng& rng) const
{
    Gather g;
    bx::Vec3 weighted{0.0f, 0.0f, 0.0f};
    float weight = 0.0f;
    const bx::Vec3 origin = bx::add(p, bx::mul(n, kRayOffset));
    const float du = rng.uniform(), dv = rng.uniform();
    for (int s = 0; s < samples; ++s) {
        const auto [u, v] = hammersley(uint32_t(s), uint32_t(samples), du, dv);
        const bx::Vec3 d = cosineDirection(n, u, v);
        bool backface = false;
        const bx::Vec3 l = radiance(origin, d, bounces, rng, backface);
        g.backfaces += backface ? 1 : 0;
        g.irradiance = bx::add(g.irradiance, l);
        weighted = bx::add(weighted, bx::mul(d, luminance(l)));
        weight += luminance(l);
        if (directFull) {
            bx::Vec3 lightWeighted{0.0f, 0.0f, 0.0f};
            const bx::Vec3 e = direct(p, n, true, rng, &lightWeighted);
            g.irradiance = bx::add(g.irradiance, e);
            weighted = bx::add(weighted, lightWeighted);
            weight += luminance(e);
        }
    }
    g.irradiance = bx::mul(g.irradiance, 1.0f / float(std::max(samples, 1)));
    g.direction = weight > 0.0f ? bx::mul(weighted, 1.0f / weight) : bx::Vec3{0.0f, 0.0f, 0.0f};
    return g;
}

bool bakeLightmaps(const LightmapBakeInput& input, LightmapBakeProgress& progress, LightmapBakeOutput& out,
                   std::string& error)
{
    const auto start = std::chrono::steady_clock::now();
    const SceneLightmaps& settings = input.settings;
    const auto cancelled = [&] {
        if (!progress.cancel.load())
            return false;
        error = "cancelled";
        return true;
    };

    // 1. One unwrap per (source, scale, density); each entity a rect of its unwrap's size.
    std::vector<LightmapMesh> meshes;
    std::vector<std::pair<int, int>> sizes;     // per unwrap
    std::map<std::string, size_t> unwrapOf;      // key -> index in meshes
    std::vector<size_t> entityUnwrap;            // per entity
    for (const BakeEntity& entity : input.entities) {
        const std::string key = unwrapKey(entity);
        auto [it, added] = unwrapOf.try_emplace(key, meshes.size());
        if (added) {
            LightmapUnwrap unwrap;
            if (!unwrapLightmapMeshes(entity.meshes, axisScale(entity.world), entity.density, unwrap, error)) {
                error = std::format("'{}': {}", entity.name, error);
                return false;
            }
            meshes.push_back({key, entity.source, std::move(unwrap.submeshes)});
            sizes.push_back({unwrap.width, unwrap.height});
        }
        entityUnwrap.push_back(it->second);
        if (cancelled())
            return false;
    }
    std::vector<std::pair<int, int>> rects;
    for (size_t u : entityUnwrap)
        rects.push_back(sizes[u]);
    std::vector<std::pair<int, int>> pages;
    const std::vector<Placement> placed = rects.empty() ? std::vector<Placement>() : packRects(rects, pages);

    // 2. Images (atlas pages, then terrain tiles) and the texels to bake, rasterized from the unwrapped triangles.
    std::vector<Image> images;
    for (const auto& [w, h] : pages)
        images.push_back({w, h});
    for (const BakeTerrain& terrain : input.terrain)
        for (size_t t = 0; t < terrain.tiles.size(); ++t)
            images.push_back({terrain.size, terrain.size});
    for (Image& image : images) {
        const size_t n = size_t(image.width) * image.height;
        image.irradiance.assign(n * 4, 0.0f);
        image.direction.assign(n * 4, 0.0f);
        image.state.assign(n, 0);
    }
    std::vector<Texel> texels;
    std::vector<int32_t> owner; // per page texel: in texels, or -1
    LightmapSidecar sidecar{.hash = input.hash};
    for (size_t e = 0; e < input.entities.size(); ++e) {
        const BakeEntity& entity = input.entities[e];
        const LightmapMesh& mesh = meshes[entityUnwrap[e]];
        const Placement& at = placed[e];
        const auto [w, h] = rects[e];
        const auto [pw, ph] = pages[at.page];
        sidecar.entities.push_back({.id = entity.id, .mesh = mesh.key, .atlas = at.page,
                                    .scaleOffset = {float(w) / float(pw), float(h) / float(ph), float(at.x) / float(pw),
                                                    float(at.y) / float(ph)}});
        Image& image = images[at.page];
        for (size_t s = 0; s < mesh.submeshes.size(); ++s) {
            const LightmapSubmesh& sub = mesh.submeshes[s];
            const BakeMesh& source = entity.meshes[s];
            for (size_t i = 0; i + 2 < sub.indices.size(); i += 3) {
                float px[3], py[3];
                bx::Vec3 wp[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, wn[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
                for (int k = 0; k < 3; ++k) {
                    const uint32_t v = sub.indices[i + k];
                    px[k] = float(at.x) + sub.uv[v * 2] * float(w);
                    py[k] = float(at.y) + sub.uv[v * 2 + 1] * float(h);
                    wp[k] = transformPoint(source.positions[sub.xref[v]], entity.world);
                    wn[k] = transformNormal(source.normals[sub.xref[v]], entity.world);
                }
                const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                if (std::abs(area) < 1e-12f)
                    continue;
                const bx::Vec3 face = bx::normalize(bx::cross(bx::sub(wp[1], wp[0]), bx::sub(wp[2], wp[0])));
                const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}))));
                const int x1 = std::min(image.width - 1, int(std::ceil(std::max({px[0], px[1], px[2]}))));
                const int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}))));
                const int y1 = std::min(image.height - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                        const float b0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
                        const float b1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
                        const float b2 = 1.0f - b0 - b1;
                        if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
                            continue;
                        const size_t index = size_t(y) * image.width + x;
                        if (image.state[index] != 0)
                            continue; // already a texel of another triangle
                        image.state[index] = 1;
                        bx::Vec3 normal = bx::normalize(
                            bx::add(bx::add(bx::mul(wn[0], b0), bx::mul(wn[1], b1)), bx::mul(wn[2], b2)));
                        if (bx::dot(normal, face) < 0.0f)
                            normal = face;
                        texels.push_back({bx::add(bx::add(bx::mul(wp[0], b0), bx::mul(wp[1], b1)), bx::mul(wp[2], b2)),
                                          normal, at.page, static_cast<uint32_t>(index)});
                    }
            }
        }
    }
    size_t terrainImage = pages.size();
    for (const BakeTerrain& terrain : input.terrain)
        for (const BakeTerrainTile& tile : terrain.tiles) {
            for (size_t i = 0; i < tile.positions.size(); ++i)
                texels.push_back({tile.positions[i], tile.normals[i], static_cast<uint32_t>(terrainImage),
                                  static_cast<uint32_t>(i)});
            std::ranges::fill(images[terrainImage].state, 1);
            ++terrainImage;
        }

    // 3. The probe grid over the Static meshes' bounds (the terrain's if there are none), its spacing grown to fit
    // kMaxGridProbes.
    const LightmapTracer tracer(input);
    Aabb bounds{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    if (!input.entities.empty()) {
        for (const BakeEntity& entity : input.entities)
            for (const BakeMesh& mesh : entity.meshes)
                for (const bx::Vec3& p : mesh.positions) {
                    const bx::Vec3 w = transformPoint(p, entity.world);
                    bounds.min = bx::min(bounds.min, w);
                    bounds.max = bx::max(bounds.max, w);
                }
    } else {
        bounds = tracer.bvh().bounds();
    }
    ProbeGrid& grid = sidecar.grid;
    grid.spacing = settings.probeSpacing;
    const bx::Vec3 extent = bx::sub(bounds.max, bounds.min);
    for (;;) {
        for (int a = 0; a < 3; ++a)
            grid.counts[a] = std::max(1, int(std::ceil((a == 0 ? extent.x : a == 1 ? extent.y : extent.z) / grid.spacing)));
        if (size_t(grid.counts[0]) * grid.counts[1] * grid.counts[2] <= kMaxGridProbes)
            break;
        grid.spacing *= 1.25f;
    }
    // Centered in the bounds: probes sit half a spacing in from its faces, off the surfaces bounding it.
    grid.origin = bx::sub(bx::mul(bx::add(bounds.min, bounds.max), 0.5f),
                          bx::mul(bx::Vec3{float(grid.counts[0] - 1), float(grid.counts[1] - 1), float(grid.counts[2] - 1)},
                                  0.5f * grid.spacing));
    const size_t probes = size_t(grid.counts[0]) * grid.counts[1] * grid.counts[2];
    grid.sh.assign(probes, Sh9{});
    grid.valid.assign(probes, 0);

    // 4. Trace every texel, then every probe, on all cores.
    std::atomic<size_t> done{0};
    const double total = double(texels.size() + probes * 4 + 1); // a probe's rays cost about 4 texels'
    std::jthread watcher([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            progress.fraction.store(float(double(done.load()) / total));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    std::vector<uint8_t> inside(texels.size(), 0);
    std::vector<std::array<float, 8>> results(texels.size());
    parallelFor(texels.size(), progress, done, [&](size_t i) {
        const Texel& texel = texels[i];
        BakeRng rng(input.seed, i);
        const LightmapTracer::Gather g =
            tracer.gather(texel.position, texel.normal, settings.samplesPerTexel, settings.bounces, true, rng);
        const float w = bx::dot(texel.normal, g.direction) * 0.5f + 0.5f;
        results[i] = {g.irradiance.x, g.irradiance.y, g.irradiance.z, 1.0f, g.direction.x, g.direction.y, g.direction.z, w};
        inside[i] = float(g.backfaces) > kTexelBackfaces * float(settings.samplesPerTexel) ? 1 : 0;
    });
    if (cancelled())
        return false;
    const int rays = std::max(kMinProbeRays, settings.samplesPerTexel);
    std::atomic<size_t> probesDone{0};
    parallelFor(probes, progress, probesDone, [&](size_t i) {
        const int ix = int(i % size_t(grid.counts[0])), iy = int(i / size_t(grid.counts[0]) % size_t(grid.counts[1]));
        const int iz = int(i / (size_t(grid.counts[0]) * grid.counts[1]));
        const bx::Vec3 p = bx::add(grid.origin, bx::mul(bx::Vec3{float(ix), float(iy), float(iz)}, grid.spacing));
        BakeRng rng(input.seed ^ 0x9e3779b97f4a7c15ull, i);
        std::vector<bx::Vec3> dirs(size_t(rays), bx::Vec3{0.0f, 0.0f, 0.0f}), radiance = dirs;
        int backfaces = 0;
        const float du = rng.uniform(), dv = rng.uniform();
        for (int r = 0; r < rays; ++r) {
            const auto [u, v] = hammersley(uint32_t(r), uint32_t(rays), du, dv);
            dirs[size_t(r)] = sphereDirection(u, v);
            bool backface = false;
            radiance[size_t(r)] = tracer.radiance(p, dirs[size_t(r)], settings.bounces, rng, backface);
            backfaces += backface ? 1 : 0;
        }
        grid.sh[i] = projectRadianceSamples(dirs, radiance);
        grid.valid[i] = float(backfaces) <= kProbeBackfaces * float(rays) ? 1 : 0;
        done.fetch_add(4, std::memory_order_relaxed);
    });
    watcher.request_stop();
    if (cancelled())
        return false;

    // 5. Results into their images, inside-geometry texels and the charts' surroundings dilated over.
    for (size_t i = 0; i < texels.size(); ++i) {
        Image& image = images[texels[i].page];
        const size_t index = texels[i].index;
        std::copy_n(results[i].begin(), 4, image.irradiance.begin() + ptrdiff_t(index * 4));
        std::copy_n(results[i].begin() + 4, 4, image.direction.begin() + ptrdiff_t(index * 4));
        image.state[index] = inside[i] ? 2 : 1;
    }
    for (Image& image : images)
        dilate(image);

    // 6. Files: the atlas pages, meshes.klmm, lightmaps.json last; each terrain map's tiles, its lightmaps.json last.
    LightmapBakeOutput result;
    for (size_t p = 0; p < pages.size(); ++p) {
        const Image& image = images[p];
        LightmapAtlas atlas{.width = static_cast<uint16_t>(image.width), .height = static_cast<uint16_t>(image.height),
                            .irradiance = std::format("atlas{}.ktx", p), .direction = std::format("atlas{}_dir.ktx", p)};
        std::string irradiance = lightmapKtx(atlas.width, atlas.height, image.irradiance);
        std::string direction = lightmapKtx(atlas.width, atlas.height, image.direction);
        atlas.irradianceHash = lightmapFileHash(irradiance);
        atlas.directionHash = lightmapFileHash(direction);
        result.files.push_back({input.folder / atlas.irradiance, std::move(irradiance)});
        result.files.push_back({input.folder / atlas.direction, std::move(direction)});
        sidecar.atlases.push_back(std::move(atlas));
    }
    std::string meshesBin = writeLightmapMeshes(meshes);
    sidecar.meshesHash = lightmapFileHash(meshesBin);
    result.files.push_back({input.folder / sidecar.meshes, std::move(meshesBin)});
    result.files.push_back({input.folder / "lightmaps.json", lightmapSidecarJson(sidecar)});
    size_t image = pages.size();
    for (const BakeTerrain& terrain : input.terrain) {
        TerrainLightmaps index{.hash = input.hash, .size = terrain.size};
        for (const BakeTerrainTile& tile : terrain.tiles) {
            const Image& img = images[image++];
            const auto size = static_cast<uint16_t>(terrain.size);
            std::string irradiance = lightmapKtx(size, size, img.irradiance);
            std::string direction = lightmapKtx(size, size, img.direction);
            index.tiles.push_back({tile.tx, tile.tz, lightmapFileHash(irradiance), lightmapFileHash(direction)});
            result.files.push_back({terrain.folder / terrainLightmapFile(tile.tx, tile.tz, false), std::move(irradiance)});
            result.files.push_back({terrain.folder / terrainLightmapFile(tile.tx, tile.tz, true), std::move(direction)});
        }
        result.files.push_back({terrain.folder / "lightmaps.json", terrainLightmapsJson(index)});
        result.terrainTiles += terrain.tiles.size();
    }
    result.atlases = pages.size();
    result.texels = texels.size();
    result.probes = probes;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    progress.fraction.store(1.0f);
    out = std::move(result);
    return true;
}

bool writeLightmapBake(const LightmapBakeOutput& out, std::string& error)
{
    for (const auto& [file, bytes] : out.files) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (!writeFileAtomic(file, bytes)) {
            error = std::format("cannot write '{}'", toUtf8(file));
            return false;
        }
    }
    return true;
}

} // namespace kast
