#include "renderer.h"

#include "fs_line.bin.h"
#include "fs_mesh.bin.h"
#include "fs_mesh_sdfgi.bin.h"
#include "fs_shadow.bin.h"
#include "vs_line.bin.h"
#include "vs_mesh.bin.h"
#include "vs_mesh_instanced.bin.h"
#include "vs_mesh_skinned.bin.h"
#include "vs_shadow.bin.h"
#include "vs_shadow_instanced.bin.h"
#include "vs_shadow_skinned.bin.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

#include "animation.h"
#include "files.h"
#include "frustum.h"
#include "log.h"
#include "physics.h"
#include "post_process.h"
#include "project.h"
#include "sdfgi.h"
#include "terrain.h"
#include "voxel_gi.h"

#include <bimg/bimg.h>
#include <bx/error.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

namespace kast {
namespace {

constexpr int kGridHalfExtent = 10;    // grid covers -10..10 m on X and Z, one line per meter
constexpr float kGridLift = 0.005f;    // ponytail: lifts the grid off y=0 to avoid z-fighting a ground plane; z-fights again beyond ~100 m
constexpr uint32_t kGridColor = 0xff505050; // ABGR
constexpr uint32_t kAxisX = 0xff3030e0;
constexpr uint32_t kAxisY = 0xff30e030;
constexpr uint32_t kAxisZ = 0xffe03030;
constexpr uint32_t kFrustumColor = 0xffe0e0e0;
constexpr float kFrustumPreviewDistance = 2.0f; // meters: a short preview, not the far plane
constexpr float kFrustumPreviewAspect = 16.0f / 9.0f; // ponytail: fixed; pass the Game panel's aspect if it matters
constexpr uint32_t kLightGizmoColor = 0xff40d0f0;
constexpr uint32_t kColliderColor = 0xff40ff60; // ABGR: green
constexpr int kRangeCircleSegments = 48;
constexpr uint32_t kParticleShapeColor = 0xff00aaff; // ABGR: the editor's orange
constexpr uint32_t kProbeBoxColor = 0xff00ccff;      // ABGR: the editor's highlight yellow
constexpr uint32_t kVoxelGiBoxColor = 0xff00aaff;    // ABGR: the editor's orange
constexpr int kSphereStacks = 16;
constexpr int kSphereSlices = 32;
// No culling here: a mesh draw adds its material's cullMode (cullState), lines draw as they are.
constexpr uint64_t kState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                            BGFX_STATE_MSAA;
// Blended meshes test depth but don't write it, so blended meshes behind them still draw.
constexpr uint64_t kBlendState = (kState & ~BGFX_STATE_WRITE_Z) | BGFX_STATE_BLEND_ALPHA;
// Additive meshes likewise, adding src * alpha to what is behind them (as additive particles do).
constexpr uint64_t kAdditiveState =
    (kState & ~BGFX_STATE_WRITE_Z) | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
// Shadow casters: depth only, both faces (open planes cast too).
constexpr uint64_t kShadowState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
constexpr uint64_t kShadowMapFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | BGFX_SAMPLER_UVW_CLAMP;
// D3D cube face order (+X, -X, +Y, -Y, +Z, -Z): the direction each face looks and its up, for left-handed views.
constexpr bx::Vec3 kCubeFaceForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
constexpr bx::Vec3 kCubeFaceUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
// A probe's faces for right-handed views of Kast's world, in bgfx's side order (+X, -X, +Y, -Y, +Z, -Z of the cube,
// whose -Z is Kast's +Z: ibl.h): each face's image as that side's texels lie, unmirrored, so winding and culling hold.
constexpr bx::Vec3 kProbeFaceForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}};
constexpr bx::Vec3 kProbeFaceUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
constexpr float kProbeNear = 0.05f; // meters: a probe's faces' near and far planes
constexpr float kProbeFar = 1000.0f;

struct LineVertex {
    float x, y, z;
    uint32_t abgr;
};


// Quad centered at center, spanning +-u and +-v, facing normal = u x v (counter-clockwise from the front). Texture u
// runs along +u and v along -v (image up is +v), so the tangent is u and the bitangent cross(normal, u) = v: w = 1.
void addQuad(MeshData& mesh, bx::Vec3 center, bx::Vec3 u, bx::Vec3 v, bx::Vec3 normal)
{
    const auto base = static_cast<uint16_t>(mesh.vertices.size());
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const bx::Vec3 t = bx::normalize(u);
    for (const auto& c : corners) {
        const bx::Vec3 p = bx::add(center, bx::add(bx::mul(u, c[0]), bx::mul(v, c[1])));
        mesh.vertices.push_back({p.x, p.y, p.z, normal.x, normal.y, normal.z, t.x, t.y, t.z, 1.0f, c[0] * 0.5f + 0.5f,
                                 0.5f - c[1] * 0.5f});
    }
    for (int i : {0, 1, 2, 0, 2, 3})
        mesh.indices.push_back(static_cast<uint16_t>(base + i));
}

MeshData makeCube()
{
    MeshData mesh;
    const bx::Vec3 x{0.5f, 0.0f, 0.0f}, y{0.0f, 0.5f, 0.0f}, z{0.0f, 0.0f, 0.5f};
    // (u, v) per face with u x v pointing outward
    const bx::Vec3 faces[6][2] = {{y, z}, {z, y}, {z, x}, {x, z}, {x, y}, {y, x}};
    for (const auto& face : faces) {
        const bx::Vec3 normal = bx::normalize(bx::cross(face[0], face[1]));
        addQuad(mesh, bx::mul(normal, 0.5f), face[0], face[1], normal);
    }
    return mesh;
}

MeshData makePlane()
{
    MeshData mesh;
    addQuad(mesh, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    return mesh;
}

MeshData makeSphere()
{
    MeshData mesh;
    for (int stack = 0; stack <= kSphereStacks; ++stack) {
        const float phi = bx::kPi * static_cast<float>(stack) / kSphereStacks;
        for (int slice = 0; slice <= kSphereSlices; ++slice) {
            const float theta = 2.0f * bx::kPi * static_cast<float>(slice) / kSphereSlices;
            const bx::Vec3 n{std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
            // Tangent: d/dtheta (texture u), defined at the poles too; image up (-v) is north, cross(n, t): w = 1.
            const bx::Vec3 t{std::cos(theta), 0.0f, -std::sin(theta)};
            mesh.vertices.push_back({n.x * 0.5f, n.y * 0.5f, n.z * 0.5f, n.x, n.y, n.z, t.x, t.y, t.z, 1.0f,
                                     static_cast<float>(slice) / kSphereSlices, static_cast<float>(stack) / kSphereStacks});
        }
    }
    constexpr int kRow = kSphereSlices + 1;
    for (int stack = 0; stack < kSphereStacks; ++stack) {
        for (int slice = 0; slice < kSphereSlices; ++slice) {
            const auto a = static_cast<uint16_t>(stack * kRow + slice);
            const auto b = static_cast<uint16_t>(a + kRow);
            for (uint16_t i : {a, b, static_cast<uint16_t>(b + 1), a, static_cast<uint16_t>(b + 1), static_cast<uint16_t>(a + 1)})
                mesh.indices.push_back(i);
        }
    }
    return mesh;
}

std::vector<LineVertex> makeGrid()
{
    std::vector<LineVertex> lines;
    const auto extent = static_cast<float>(kGridHalfExtent);
    for (int i = -kGridHalfExtent; i <= kGridHalfExtent; ++i) {
        if (i == 0)
            continue; // the axes take the center lines
        const auto f = static_cast<float>(i);
        lines.push_back({f, kGridLift, -extent, kGridColor});
        lines.push_back({f, kGridLift, extent, kGridColor});
        lines.push_back({-extent, kGridLift, f, kGridColor});
        lines.push_back({extent, kGridLift, f, kGridColor});
    }
    lines.push_back({-extent, kGridLift, 0.0f, kAxisX});
    lines.push_back({extent, kGridLift, 0.0f, kAxisX});
    lines.push_back({0.0f, 0.0f, 0.0f, kAxisY});
    lines.push_back({0.0f, extent, 0.0f, kAxisY});
    lines.push_back({0.0f, kGridLift, -extent, kAxisZ});
    lines.push_back({0.0f, kGridLift, extent, kAxisZ});
    return lines;
}

template <typename T>
const bgfx::Memory* copyOf(const std::vector<T>& data)
{
    return bgfx::copy(data.data(), static_cast<uint32_t>(data.size() * sizeof(T)));
}

// transpose(inverse(world)): u_normalMatrix (vs_mesh.sc).
void normalMatrix(const Mat4& world, float* out)
{
    float inverse[16];
    bx::mtxInverse(inverse, world.data());
    bx::mtxTranspose(out, inverse);
}

bgfx::ProgramHandle makeProgram(const uint8_t* vs, uint32_t vsSize, const uint8_t* fs, uint32_t fsSize)
{
    return bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs, vsSize)), bgfx::createShader(bgfx::makeRef(fs, fsSize)),
                               true);
}

bgfx::TextureHandle createTexture(const MipImage& image, bool srgb)
{
    return bgfx::createTexture2D(image.width, image.height, image.levels > 1, 1, bgfx::TextureFormat::RGBA8,
                                 srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE,
                                 bgfx::copy(image.rgba.data(), static_cast<uint32_t>(image.rgba.size())));
}

// Draws lines (vertex pairs) with this frame's transient buffer; skipped if it has no room left.
void submitLines(const std::vector<LineVertex>& lines, const bgfx::VertexLayout& layout, bgfx::ProgramHandle program,
                 bgfx::ViewId view)
{
    const auto count = static_cast<uint32_t>(lines.size());
    if (count == 0 || bgfx::getAvailTransientVertexBuffer(count, layout) < count)
        return;
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, count, layout);
    std::memcpy(buffer.data, lines.data(), count * sizeof(LineVertex));
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::setState(kState | BGFX_STATE_PT_LINES);
    bgfx::submit(view, program);
}

// World-space box around a local box transformed by world.
Aabb transformBounds(const Aabb& local, const float* world)
{
    Aabb result{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    for (int corner = 0; corner < 8; ++corner) {
        const bx::Vec3 p = bx::mul(bx::Vec3{corner & 1 ? local.max.x : local.min.x, corner & 2 ? local.max.y : local.min.y,
                                            corner & 4 ? local.max.z : local.min.z},
                                   world);
        result.min = bx::min(result.min, p);
        result.max = bx::max(result.max, p);
    }
    return result;
}

// What draws must share to be one instanced submit: buffers, material, shadow flags and, in the main pass, the local
// lights that reach them, the environment they take their ambient from and their diffuse GI (gi: 0 none, else a
// lightmap's texture + 1, or a probe-grid draw's own number, plus its VoxelGI volume + 1 << 40: so each instance is lit
// as its own draw would be).
struct BatchKey {
    uint16_t vertices = 0, indices = 0;
    const Material* material = nullptr;
    bool skinned = false, castShadows = false, receiveShadows = false;
    bool mirrored = false; // culls the other winding (cullState)
    std::vector<size_t> lights;
    size_t environment = 0;
    size_t gi = 0;
    auto operator<=>(const BatchKey&) const = default;
};

template <typename Item> // Renderer::DrawItem
BatchKey batchKey(const Item& item, std::vector<size_t> lights, size_t environment = 0, size_t gi = 0)
{
    return {item.vertices.idx, item.indices.idx, item.material, item.bones >= 0, item.castShadows, item.receiveShadows,
            item.mirrored, std::move(lights), environment, gi};
}

// Indices 0..count-1 grouped by keyOf(index): groups in order of first appearance, indices in order within each.
template <typename KeyOf>
std::vector<std::vector<size_t>> batchDraws(size_t count, KeyOf keyOf)
{
    std::map<BatchKey, size_t> group;
    std::vector<std::vector<size_t>> batches;
    for (size_t i = 0; i < count; ++i) {
        const auto [it, added] = group.try_emplace(keyOf(i), batches.size());
        if (added)
            batches.emplace_back();
        batches[it->second].push_back(i);
    }
    return batches;
}

// World matrices for one frame, each ancestor's computed once (Scene::worldMatrix walks the whole parent chain per
// call). Same math as Scene::worldMatrix, so the same result.
class WorldMatrices {
public:
    explicit WorldMatrices(const Scene& scene) : scene_(scene) {}

    Mat4 operator()(entt::entity entity)
    {
        const size_t index = entt::to_entity(entity);
        if (index < done_.size() && done_[index])
            return cache_[index];
        Mat4 world;
        scene_.localMatrix(entity, world.data());
        if (const entt::entity parent = scene_.parent(entity); parent != entt::null) {
            const Mat4 local = world;
            bx::mtxMul(world.data(), local.data(), (*this)(parent).data());
        }
        if (index >= done_.size()) {
            done_.resize(index + 1, false);
            cache_.resize(index + 1);
        }
        done_[index] = true;
        return cache_[index] = world;
    }

private:
    const Scene& scene_;
    std::vector<Mat4> cache_; // by entity index
    std::vector<bool> done_;
};

// The file a texture path reads: a model's embedded image "<model>#<i>" comes from the model file; an ORM pair's
// (ormSources) is its metallic-roughness image's. ponytail: so editing only its occlusion image doesn't reload the pair.
std::string textureFile(const std::string& path)
{
    if (const auto orm = ormSources(path))
        return textureFile(orm->first);
    const std::string model = embeddedTextureModel(path);
    return model.empty() ? path : model;
}

// The cull flag for a material's cullMode. Front faces wind counter-clockwise (addQuad, glTF), so with a view that
// keeps winding they reach the screen counter-clockwise: back = cull clockwise. A mirroring world matrix (negative
// determinant) reverses the winding, so the flags swap.
uint64_t cullState(CullMode mode, bool mirrored)
{
    if (mode == CullMode::None)
        return 0;
    return (mode == CullMode::Back) != mirrored ? BGFX_STATE_CULL_CW : BGFX_STATE_CULL_CCW;
}

bool isMirrored(const Mat4& world)
{
    const bx::Vec3 x{world[0], world[1], world[2]}, y{world[4], world[5], world[6]}, z{world[8], world[9], world[10]};
    return bx::dot(bx::cross(x, y), z) < 0.0f;
}

// Blended and additive meshes: drawn after the opaque ones, far to near, and never into the shadow maps.
bool isTransparent(const Material& material)
{
    return material.alphaMode == AlphaMode::Blend || material.alphaMode == AlphaMode::Additive;
}

} // namespace

MeshData primitiveMesh(Primitive primitive)
{
    switch (primitive) {
    case Primitive::Cube:
        return makeCube();
    case Primitive::Plane:
        return makePlane();
    case Primitive::Sphere:
        break;
    }
    return makeSphere();
}

std::string assetLoadSummary()
{
    const AssetLoadStats& s = assetLoadStats();
    const bgfx::Stats* stats = bgfx::getStats();
    return std::format("Asset loads: {} models in {:.1f} ms, {} textures in {:.1f} ms; textureMemoryUsed {:.2f} MiB",
                       s.models, s.modelSeconds * 1000.0, s.textures, s.textureSeconds * 1000.0,
                       stats ? static_cast<double>(stats->textureMemoryUsed) / (1024.0 * 1024.0) : 0.0);
}

std::optional<MipImage> decodeImage(std::string_view bytes, bool srgb)
{
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                            &width, &height, &channels, 4);
    if (!pixels)
        return std::nullopt;
    if (width > 16384 || height > 16384) { // bgfx sizes are uint16_t, and no GPU takes more
        stbi_image_free(pixels);
        return std::nullopt;
    }
    MipImage image{static_cast<uint16_t>(width), static_cast<uint16_t>(height), 1, {}};
    image.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    buildMips(image, srgb);
    return image;
}

bool isGpuImage(std::string_view bytes, std::string_view path)
{
    const auto extension = [&](std::string_view e) {
        return path.size() >= e.size() && std::ranges::equal(path.substr(path.size() - e.size()), e, [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               });
    };
    return bytes.starts_with("DDS ") || bytes.starts_with("\xABKTX") || extension(".ktx") || extension(".dds");
}

std::optional<GpuImageInfo> parseGpuImage(std::string_view bytes, bool cube, std::string& error)
{
    bimg::ImageContainer image{};
    bx::Error err;
    if (bytes.size() > UINT32_MAX || !bimg::imageParse(image, bytes.data(), static_cast<uint32_t>(bytes.size()), &err) ||
        image.m_format >= bimg::TextureFormat::Count) {
        error = "not a valid KTX or DDS file";
        return std::nullopt;
    }
    // Only the header was read: the mips it promises must all be in the file (bimg asserts in debug builds otherwise).
    // KTX prefixes each mip with its u32 size; padding is not counted, so this is a lower bound.
    const bimg::ImageBlockInfo& block = bimg::getBlockInfo(image.m_format);
    uint64_t need = 0;
    for (uint32_t mip = 0; mip < image.m_numMips; ++mip) {
        const uint64_t w = std::max<uint64_t>(image.m_width >> mip, 1), h = std::max<uint64_t>(image.m_height >> mip, 1);
        const uint64_t blocks = std::max<uint64_t>((w + block.blockWidth - 1) / block.blockWidth, block.minBlockX) *
                                std::max<uint64_t>((h + block.blockHeight - 1) / block.blockHeight, block.minBlockY);
        need += blocks * block.blockSize * (image.m_cubeMap ? 6 : 1) * std::max<uint32_t>(image.m_numLayers, 1) +
                (image.m_ktx ? 4 : 0);
    }
    if (image.m_numMips == 0 || image.m_width == 0 || image.m_height == 0 || image.m_offset + need > bytes.size()) {
        error = "the file is truncated";
        return std::nullopt;
    }
    if (image.m_depth > 1 || image.m_numLayers > 1) {
        error = "3D and array textures are not supported";
        return std::nullopt;
    }
    if (image.m_cubeMap != cube) {
        error = cube ? "a 2D texture, not a cubemap" : "a cubemap, not a 2D texture";
        return std::nullopt;
    }
    if (image.m_width > 16384 || image.m_height > 16384) {
        error = std::format("{}x{} is too large", image.m_width, image.m_height);
        return std::nullopt;
    }
    return GpuImageInfo{static_cast<bgfx::TextureFormat::Enum>(image.m_format), static_cast<uint16_t>(image.m_width),
                        static_cast<uint16_t>(image.m_height), image.m_numMips, image.m_cubeMap};
}

bgfx::TextureHandle createGpuTexture(std::string_view bytes, bool srgb, bool cube, uint64_t flags, std::string& error)
{
    const std::optional<GpuImageInfo> info = parseGpuImage(bytes, cube, error);
    if (!info)
        return BGFX_INVALID_HANDLE;
    const uint32_t need = cube ? (srgb ? BGFX_CAPS_FORMAT_TEXTURE_CUBE_SRGB : BGFX_CAPS_FORMAT_TEXTURE_CUBE)
                               : (srgb ? BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB : BGFX_CAPS_FORMAT_TEXTURE_2D);
    if (!(bgfx::getCaps()->formats[info->format] & need)) {
        error = std::format("this GPU has no {}{} textures", bimg::getName(static_cast<bimg::TextureFormat::Enum>(info->format)),
                            srgb ? " sRGB" : "");
        return BGFX_INVALID_HANDLE;
    }
    // bgfx parses the KTX / DDS itself and uploads each side's mips as stored.
    return bgfx::createTexture(bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size())),
                               flags | (srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE));
}

void buildMips(MipImage& image, bool srgb)
{
    image.levels = 1;
    // Each level is filtered from the one before; sRGB images are filtered in linear space so the mips don't darken.
    size_t level = 0; // byte offset of the last level
    for (int w = image.width, h = image.height; w > 1 || h > 1; ++image.levels) {
        const int nw = bx::max(w / 2, 1), nh = bx::max(h / 2, 1);
        const size_t next = image.rgba.size();
        image.rgba.resize(next + static_cast<size_t>(nw) * nh * 4); // may move the data: take pointers after
        if (srgb)
            stbir_resize_uint8_srgb(image.rgba.data() + level, w, h, 0, image.rgba.data() + next, nw, nh, 0, STBIR_RGBA);
        else
            stbir_resize_uint8_linear(image.rgba.data() + level, w, h, 0, image.rgba.data() + next, nw, nh, 0, STBIR_RGBA);
        level = next;
        w = nw;
        h = nh;
    }
}

float pointLightFalloff(float distance, float range)
{
    const float ratio = distance / range;
    const float window = bx::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
    return window * window / (distance * distance + 1.0f);
}

float spotConeFalloff(float cosAngle, float cosInner, float cosOuter)
{
    const float t = bx::clamp((cosAngle - cosOuter) / std::max(cosInner - cosOuter, 1e-4f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool lightReaches(const LocalLightSample& light, const Aabb& bounds)
{
    const bx::Vec3 closest = bx::clamp(light.position, bounds.min, bounds.max);
    if (bx::length(bx::sub(light.position, closest)) >= light.range)
        return false;
    if (light.type != LocalLightType::Spot)
        return true;
    // The box's bounding sphere against the outer cone (Wronski's cone / sphere test): its center's distance from the
    // cone's surface, and whether it is behind the apex.
    const bx::Vec3 center = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    const float radius = 0.5f * bx::length(bx::sub(bounds.max, bounds.min));
    const bx::Vec3 toCenter = bx::sub(center, light.position);
    const float along = bx::dot(toCenter, light.direction);
    const float across = std::sqrt(std::max(bx::dot(toCenter, toCenter) - along * along, 0.0f));
    const float sinOuter = std::sqrt(std::max(1.0f - light.cosOuter * light.cosOuter, 0.0f));
    return light.cosOuter * across - along * sinOuter <= radius && along >= -radius;
}

std::vector<size_t> nearestLocalLights(const std::vector<LocalLightSample>& lights, const Aabb& bounds)
{
    const bx::Vec3 center = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    std::vector<std::pair<float, size_t>> reaching; // (squared distance to center, index)
    for (size_t i = 0; i < lights.size(); ++i)
        if (lightReaches(lights[i], bounds))
            reaching.emplace_back(bx::distanceSq(lights[i].position, center), i);
    const size_t count = std::min(reaching.size(), kMaxLocalLights);
    std::partial_sort(reaching.begin(), reaching.begin() + static_cast<ptrdiff_t>(count), reaching.end());
    std::vector<size_t> indices;
    for (size_t i = 0; i < count; ++i)
        indices.push_back(reaching[i].second);
    return indices;
}

std::vector<size_t> shadowedLights(const std::vector<LocalLightSample>& lights, const bx::Vec3& eye, bool cubes)
{
    std::vector<std::pair<float, size_t>> casting; // (squared distance to eye, index)
    for (size_t i = 0; i < lights.size(); ++i)
        if (lights[i].castShadows && (lights[i].type == LocalLightType::Point) == cubes)
            casting.emplace_back(bx::distanceSq(lights[i].position, eye), i);
    const size_t count = std::min(casting.size(), cubes ? kMaxShadowedPointLights : kMaxShadowedLocalLights);
    std::partial_sort(casting.begin(), casting.begin() + static_cast<ptrdiff_t>(count), casting.end());
    std::vector<size_t> indices;
    for (size_t i = 0; i < count; ++i)
        indices.push_back(casting[i].second);
    return indices;
}

std::span<const float> ltcTable()
{
    static const float table[] = {
#include "shaders/ltc_table.sh"
    };
    static_assert(std::size(table) == static_cast<size_t>(kLtcSize * kLtcSize * 8), "ltc_table.sh is LTC_SIZE^2 entries");
    return table;
}

float rectFormFactor(const std::array<bx::Vec3, 4>& corners, const bx::Vec3& normal, bool twoSided)
{
    // The normal's frame (z along it), then fs_mesh.sc ltcIntegrate: each edge above the horizon integrated, edges
    // crossing it cut there, the cut points joined along it.
    const bx::Vec3 t1 = bx::normalize(bx::cross(std::abs(normal.y) < 0.99f ? bx::Vec3{0.0f, 1.0f, 0.0f}
                                                                             : bx::Vec3{1.0f, 0.0f, 0.0f}, normal));
    const bx::Vec3 t2 = bx::cross(normal, t1);
    const auto edge = [](bx::Vec3 a, bx::Vec3 b) { // a, b not unit: the z of their edge's vector form factor * 2 pi
        a = bx::normalize(a);
        b = bx::normalize(b);
        const float x = bx::clamp(bx::dot(a, b), -1.0f, 1.0f);
        const float ratio = x > 0.99999f ? 1.0f : std::acos(x) / std::sqrt(std::max(1.0f - x * x, 1e-12f));
        return bx::cross(a, b).z * ratio;
    };
    std::array<bx::Vec3, 4> local = corners;
    for (bx::Vec3& c : local)
        c = {bx::dot(t1, c), bx::dot(t2, c), bx::dot(normal, c)};
    float sum = 0.0f;
    bx::Vec3 exit{0.0f, 0.0f, 0.0f}, entry{0.0f, 0.0f, 0.0f};
    bool clipped = false;
    for (size_t i = 0; i < 4; ++i) {
        const bx::Vec3 a = local[i], b = local[(i + 1) % 4];
        if (a.z >= 0.0f && b.z >= 0.0f) {
            sum += edge(a, b);
            continue;
        }
        if (a.z < 0.0f && b.z < 0.0f)
            continue;
        const bx::Vec3 cut = bx::add(a, bx::mul(bx::sub(b, a), a.z / (a.z - b.z)));
        clipped = true;
        if (a.z >= 0.0f) {
            sum += edge(a, cut);
            exit = cut;
        } else {
            sum += edge(cut, b);
            entry = cut;
        }
    }
    if (clipped)
        sum += edge(exit, entry);
    sum /= 2.0f * bx::kPi;
    return twoSided ? std::abs(sum) : std::max(sum, 0.0f);
}

struct Renderer::VoxelGiState {
    // A baked volume's files as loaded: its sidecar and its voxels on the GPU (volume.voxels 0 and error set: unusable).
    struct File {
        std::string error;
        std::optional<std::filesystem::file_time_type> jsonTime, kvoxTime;
        VoxelGiSidecar sidecar;
        VoxelGiRenderer::Volume volume;
    };
    VoxelGiRenderer passes;
    std::map<std::pair<std::string, int>, File> files; // by (bakedPath, resolution)
    bgfx::UniformHandle params = BGFX_INVALID_HANDLE;  // u_voxelGi[4]
    bgfx::UniformHandle cones = BGFX_INVALID_HANDLE;   // u_voxelCones
    bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE; // s_voxelGi (stage 11)

    void clear()
    {
        for (auto& [key, file] : files)
            passes.destroy(file.volume);
        files.clear();
    }

    // volume.bakedPath's files (set), loaded or reloaded first.
    const File& file(const std::filesystem::path& root, const VoxelGI& volume)
    {
        std::string error = "no project";
        const std::optional<std::filesystem::path> json =
            root.empty() ? std::nullopt : resolveProjectPath(root, volume.bakedPath, error);
        const std::filesystem::path kvox = json ? std::filesystem::path(*json).replace_extension(".kvox") : std::filesystem::path();
        const auto jsonTime = json ? modifiedTime(*json) : std::nullopt;
        const auto kvoxTime = json ? modifiedTime(kvox) : std::nullopt;
        auto [it, added] = files.try_emplace({volume.bakedPath, volume.resolution});
        File& f = it->second;
        if (!added && f.jsonTime == jsonTime && f.kvoxTime == kvoxTime) // a re-bake rewrites both
            return f;
        const LoadTimer timer(assetLoadStats().textures, assetLoadStats().textureSeconds);
        passes.destroy(f.volume);
        f = File{};
        f.jsonTime = jsonTime;
        f.kvoxTime = kvoxTime;
        std::string text, bytes;
        std::vector<Voxel> voxels;
        if (json && !readFile(*json, text))
            error = "cannot read the file";
        else if (json && !readFile(kvox, bytes))
            error = "cannot read its .kvox";
        else if (json && checkVoxelGiBake(text, bytes, volume.resolution, f.sidecar, voxels, error) &&
                 !passes.create(f.volume, f.sidecar.grid, voxels))
            error = "this GPU can't run VoxelGI (no compute or 3D images)";
        else if (json && bgfx::isValid(f.volume.data))
            error.clear();
        if (!error.empty()) {
            f.error = error;
            passes.destroy(f.volume);
        } else {
            log::info(std::format("Loaded VoxelGI '{}' ({} voxels, {}x{}x{})", volume.bakedPath, f.sidecar.voxels,
                                  f.sidecar.grid.dims[0], f.sidecar.grid.dims[1], f.sidecar.grid.dims[2]));
        }
        return f;
    }
};

Renderer::Renderer() : voxelGi_(std::make_unique<VoxelGiState>()), sdfgi_(std::make_unique<SdfgiRenderer>()) {}
Renderer::~Renderer() = default;

void Renderer::init()
{
    meshLayout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
        .end();
    static_assert(sizeof(MeshVertex) == 14 * sizeof(float) + 4, "MeshVertex must match meshLayout_");
    for (size_t i = 0; i < meshes_.size(); ++i) {
        const MeshData data = primitiveMesh(static_cast<Primitive>(i));
        meshes_[i].vertices = bgfx::createVertexBuffer(copyOf(data.vertices), meshLayout_);
        meshes_[i].indices = bgfx::createIndexBuffer(copyOf(data.indices));
    }
    std::vector<uint32_t> terrainIndices;
    terrainIndices.reserve(size_t(map::kTileSamples - 1) * (map::kTileSamples - 1) * 6);
    map::for_each_triangle([&](uint32_t a, uint32_t b, uint32_t c) { terrainIndices.insert(terrainIndices.end(), {a, b, c}); });
    terrainIndices_ = bgfx::createIndexBuffer(copyOf(terrainIndices), BGFX_BUFFER_INDEX32);

    modelLayout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Indices, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Weight, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    static_assert(sizeof(ModelVertex) == 20 * sizeof(float) + 4, "ModelVertex must match modelLayout_");

    lineLayout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    grid_ = bgfx::createVertexBuffer(copyOf(makeGrid()), lineLayout_);

    meshVertex_ = bgfx::createShader(bgfx::makeRef(vs_mesh, sizeof(vs_mesh)));
    skinnedVertex_ = bgfx::createShader(bgfx::makeRef(vs_mesh_skinned, sizeof(vs_mesh_skinned)));
    instancedVertex_ = bgfx::createShader(bgfx::makeRef(vs_mesh_instanced, sizeof(vs_mesh_instanced)));
    const bgfx::ShaderHandle meshFragment = bgfx::createShader(bgfx::makeRef(fs_mesh, sizeof(fs_mesh)));
    meshProgram_ = bgfx::createProgram(meshVertex_, meshFragment);
    skinnedProgram_ = bgfx::createProgram(skinnedVertex_, meshFragment);
    instancedProgram_ = bgfx::createProgram(instancedVertex_, meshFragment);
    const bgfx::ShaderHandle sdfgiFragment = bgfx::createShader(bgfx::makeRef(fs_mesh_sdfgi, sizeof(fs_mesh_sdfgi)));
    sdfgiPrograms_ = {bgfx::createProgram(meshVertex_, sdfgiFragment), bgfx::createProgram(skinnedVertex_, sdfgiFragment),
                      bgfx::createProgram(instancedVertex_, sdfgiFragment)};
    bgfx::destroy(meshFragment); // the programs keep them
    bgfx::destroy(sdfgiFragment);
    lineProgram_ = makeProgram(vs_line, sizeof(vs_line), fs_line, sizeof(fs_line));
    shadowProgram_ = makeProgram(vs_shadow, sizeof(vs_shadow), fs_shadow, sizeof(fs_shadow));
    shadowSkinnedProgram_ = makeProgram(vs_shadow_skinned, sizeof(vs_shadow_skinned), fs_shadow, sizeof(fs_shadow));
    shadowInstancedProgram_ = makeProgram(vs_shadow_instanced, sizeof(vs_shadow_instanced), fs_shadow, sizeof(fs_shadow));
    normalMatrix_ = bgfx::createUniform("u_normalMatrix", bgfx::UniformType::Mat4);
    bones_ = bgfx::createUniform("u_bones", bgfx::UniformType::Mat4, static_cast<uint16_t>(kMaxBones));
    baseColor_ = bgfx::createUniform("u_baseColor", bgfx::UniformType::Vec4);
    lightDir_ = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);
    lightColor_ = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
    ambient_ = bgfx::createUniform("u_ambient", bgfx::UniformType::Vec4);
    localPosRange_ = bgfx::createUniform("u_localPosRange", bgfx::UniformType::Vec4, kMaxLocalLights);
    localColor_ = bgfx::createUniform("u_localColor", bgfx::UniformType::Vec4, kMaxLocalLights);
    localAxis_ = bgfx::createUniform("u_localAxis", bgfx::UniformType::Vec4, kMaxLocalLights);
    emissive_ = bgfx::createUniform("u_emissive", bgfx::UniformType::Vec4);
    materialParams_ = bgfx::createUniform("u_material", bgfx::UniformType::Vec4);
    materialExt_ = bgfx::createUniform("u_materialExt", bgfx::UniformType::Vec4);
    uvTransform_ = bgfx::createUniform("u_uvTransform", bgfx::UniformType::Vec4);
    time_ = bgfx::createUniform("u_time", bgfx::UniformType::Vec4);
    cameraPos_ = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);
    fogColor_ = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);
    fogParams_ = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4);
    baseColorSampler_ = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
    metallicRoughnessSampler_ = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
    normalSampler_ = bgfx::createUniform("s_normal", bgfx::UniformType::Sampler);
    envSampler_ = bgfx::createUniform("s_env", bgfx::UniformType::Sampler);
    materialOrm_ = bgfx::createUniform("u_materialOrm", bgfx::UniformType::Vec4);
    envSh_ = bgfx::createUniform("u_envSh", bgfx::UniformType::Vec4, 9);
    envSpecular_ = bgfx::createUniform("u_envSpecular", bgfx::UniformType::Vec4);
    envBox_ = bgfx::createUniform("u_envBox", bgfx::UniformType::Vec4, 2);
    gi_ = bgfx::createUniform("u_gi", bgfx::UniformType::Vec4);
    giSh_ = bgfx::createUniform("u_giSh", bgfx::UniformType::Vec4, 9);
    lightmapST_ = bgfx::createUniform("u_lightmapST", bgfx::UniformType::Vec4);
    lightmapSampler_ = bgfx::createUniform("s_lightmap", bgfx::UniformType::Sampler);
    lightmapDirSampler_ = bgfx::createUniform("s_lightmapDir", bgfx::UniformType::Sampler);
    emissiveSampler_ = bgfx::createUniform("s_emissive", bgfx::UniformType::Sampler);
    csmMtx_ = bgfx::createUniform("u_csmMtx", bgfx::UniformType::Mat4, kMaxShadowCascades + kMaxShadowedLocalLights);
    csmSplits_ = bgfx::createUniform("u_csmSplits", bgfx::UniformType::Vec4);
    csmBias_ = bgfx::createUniform("u_csmBias", bgfx::UniformType::Vec4, kMaxShadowCascades);
    shadowParams_ = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);
    localShadow_ = bgfx::createUniform("u_localShadow", bgfx::UniformType::Vec4, kMaxLocalLights);
    csmSampler_ = bgfx::createUniform("s_csm", bgfx::UniformType::Sampler);
    kastParams_ = bgfx::createUniform("u_kastParams", bgfx::UniformType::Vec4, kMaxShaderParams);
    for (size_t i = 0; i < kMaxShaderTextures; ++i)
        shaderTextureSamplers_[i] = bgfx::createUniform(std::format("s_kastTexture{}", i).c_str(), bgfx::UniformType::Sampler);
    screenSampler_ = bgfx::createUniform("s_kastScreen", bgfx::UniformType::Sampler);
    depthSampler_ = bgfx::createUniform("s_kastDepth", bgfx::UniformType::Sampler);
    if (bgfx::getCaps()->limits.maxTextureSamplers < kMaxTextureSamplers)
        log::error(std::format("This GPU takes {} textures per draw; .kshader texture2D params need {}",
                               bgfx::getCaps()->limits.maxTextureSamplers, kMaxTextureSamplers));
    for (size_t i = 0; i < kMaxShadowedPointLights; ++i) {
        pointShadowSamplers_[i] = bgfx::createUniform(std::format("s_pointShadow{}", i).c_str(), bgfx::UniformType::Sampler);
        cubes_[i] = BGFX_INVALID_HANDLE;
        cubeFrameBuffers_[i].fill(BGFX_INVALID_HANDLE);
    }
    noShadow2D_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::D16, kShadowMapFlags);
    noShadowCube_ = bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::D16, kShadowMapFlags);
    const uint32_t white = 0xffffffff;
    white_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE,
                                   bgfx::copy(&white, sizeof(white)));
    const uint32_t flat = 0xffff8080; // ABGR: (0.5, 0.5, 1), the tangent-space normal (0, 0, 1)
    flatNormal_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE,
                                        bgfx::copy(&flat, sizeof(flat)));
    const uint32_t black = 0xff000000;
    black_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, bgfx::copy(&black, sizeof(black)));
    const uint32_t blackSides[6] = {black, black, black, black, black, black};
    blackCube_ = bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE,
                                         bgfx::copy(blackSides, sizeof(blackSides)));
    sky_.init();
    envFilter_.init();
    particles_.init(bgfx::getCaps()->supported);
    voxelGi_->passes.init();
    sdfgi_->init();
    voxelGi_->params = bgfx::createUniform("u_voxelGi", bgfx::UniformType::Vec4, 4);
    voxelGi_->cones = bgfx::createUniform("u_voxelCones", bgfx::UniformType::Vec4, kMaxVoxelGiCones);
    voxelGi_->sampler = bgfx::createUniform("s_voxelGi", bgfx::UniformType::Sampler);
}

void Renderer::shutdown()
{
    for (Mesh& mesh : meshes_) {
        bgfx::destroy(mesh.vertices);
        bgfx::destroy(mesh.indices);
    }
    clearTerrain();
    bgfx::destroy(terrainIndices_);
    bgfx::destroy(grid_);
    for (auto& [path, shader] : userShaders_)
        destroy(shader);
    userShaders_.clear();
    bgfx::destroy(meshProgram_);
    bgfx::destroy(skinnedProgram_);
    bgfx::destroy(instancedProgram_);
    for (bgfx::ProgramHandle program : sdfgiPrograms_)
        bgfx::destroy(program);
    for (bgfx::ShaderHandle shader : {meshVertex_, skinnedVertex_, instancedVertex_})
        bgfx::destroy(shader);
    bgfx::destroy(lineProgram_);
    bgfx::destroy(shadowProgram_);
    bgfx::destroy(shadowSkinnedProgram_);
    bgfx::destroy(shadowInstancedProgram_);
    for (bgfx::UniformHandle uniform : {normalMatrix_, bones_, baseColor_, lightDir_, lightColor_, ambient_, localPosRange_,
                                        localColor_, localAxis_, emissive_, materialParams_, materialExt_, uvTransform_,
                                        time_, cameraPos_, fogColor_, fogParams_, baseColorSampler_,
                                        metallicRoughnessSampler_, normalSampler_, envSampler_, emissiveSampler_,
                                        materialOrm_, envSh_, envSpecular_, envBox_, csmMtx_, csmSplits_,
                                        csmBias_, shadowParams_, localShadow_, csmSampler_, kastParams_, screenSampler_,
                                        depthSampler_, gi_, giSh_, lightmapST_, lightmapSampler_, lightmapDirSampler_})
        bgfx::destroy(uniform);
    for (bgfx::UniformHandle uniform : pointShadowSamplers_)
        bgfx::destroy(uniform);
    for (bgfx::UniformHandle uniform : shaderTextureSamplers_)
        bgfx::destroy(uniform);
    // Frame buffers first: they don't own their textures (destroyTexture = false).
    for (const auto& faces : cubeFrameBuffers_)
        for (bgfx::FrameBufferHandle frameBuffer : faces)
            if (bgfx::isValid(frameBuffer))
                bgfx::destroy(frameBuffer);
    for (bgfx::TextureHandle cube : cubes_)
        if (bgfx::isValid(cube))
            bgfx::destroy(cube);
    bgfx::destroy(noShadow2D_);
    bgfx::destroy(noShadowCube_);
    if (bgfx::isValid(csmFrameBuffer_)) {
        bgfx::destroy(csmFrameBuffer_);
        bgfx::destroy(csm_);
    }
    clearTextures();
    clearModels();
    if (bgfx::isValid(previewTexture_))
        bgfx::destroy(previewTexture_);
    previewTexture_ = BGFX_INVALID_HANDLE;
    bgfx::destroy(white_);
    bgfx::destroy(flatNormal_);
    bgfx::destroy(black_);
    bgfx::destroy(blackCube_);
    destroyCopies();
    clearProbes();
    clearLightmaps();
    if (bgfx::isValid(skyEnv_))
        bgfx::destroy(skyEnv_);
    skyEnv_ = BGFX_INVALID_HANDLE;
    skyEnvKey_.reset();
    envFilter_.shutdown();
    sky_.shutdown();
    particles_.shutdown();
    voxelGi_->clear();
    for (bgfx::UniformHandle uniform : {voxelGi_->params, voxelGi_->cones, voxelGi_->sampler})
        bgfx::destroy(uniform);
    voxelGi_->passes.shutdown();
    sdfgi_->shutdown();
}

// ponytail: full-res tiles (66049 vertices, 131072 triangles each); LOD and skirts when the draw distance grows.
void Renderer::addTerrainDraws(const Scene& scene, std::vector<DrawItem>& draws)
{
    const entt::registry& registry = scene.registry();
    for (auto [entity, tile] : registry.view<TerrainTile>().each()) {
        if (!tile.data)
            continue;
        const TerrainTileData& data = *tile.data;
        const map::MapInfo& info = *data.map;
        auto [it, added] = terrain_.try_emplace(tile.data.get());
        TerrainMesh& mesh = it->second;
        mesh.used = true;
        if (added) {
            constexpr int n = map::kTileSamples;
            // UV per tile sample: tiled = local / 8 (world xz / 8; tile-local gives the same wrap, 256 / 8 being whole,
            // without float drift far out); map = 0..1 over the declared tiles' bounding box.
            // ponytail: the sampler repeats, so map mode's outer pixel blends with the opposite edge; clamp if it shows.
            int uvOrigin[2] = {0, 0}; // in samples, so shared edges get bit-equal UVs
            float uvScale[2] = {1.0f / 8.0f, 1.0f / 8.0f};
            if (info.terrainUv == map::TerrainUv::Map) {
                int x0 = INT_MAX, z0 = INT_MAX, x1 = INT_MIN, z1 = INT_MIN;
                for (const map::TileRect& r : info.tiles) {
                    x0 = std::min(x0, r.x0), z0 = std::min(z0, r.z0), x1 = std::max(x1, r.x1), z1 = std::max(z1, r.z1);
                }
                uvScale[0] = 1.0f / (float(x1 - x0 + 1) * map::kTileSize);
                uvScale[1] = 1.0f / (float(z1 - z0 + 1) * map::kTileSize);
                uvOrigin[0] = (data.tile.x - x0) * int(map::kTileSize);
                uvOrigin[1] = (data.tile.z - z0) * int(map::kTileSize);
            }
            std::vector<MeshVertex> vertices;
            vertices.reserve(size_t(n) * n);
            float lo = FLT_MAX, hi = -FLT_MAX;
            for (int v = 0; v < n; ++v)
                for (int u = 0; u < n; ++u) {
                    const float y = map::sample_height(data.heights[size_t(v) * n + size_t(u)], info);
                    const auto normal = map::normal_at(data.heights, info, u, v);
                    const bx::Vec3 t = terrainTangent({normal[0], normal[1], normal[2]});
                    // TEXCOORD1: the tile's lightmap, one texel per sample, centered on it (lightmap_bake.h).
                    vertices.push_back({float(u), y, float(v), normal[0], normal[1], normal[2], t.x, t.y, t.z, 1.0f,
                                        float(uvOrigin[0] + u) * uvScale[0], float(uvOrigin[1] + v) * uvScale[1],
                                        0xffffffff, (float(u) + 0.5f) / float(n), (float(v) + 0.5f) / float(n)});
                    lo = std::min(lo, y);
                    hi = std::max(hi, y);
                }
            mesh.data = tile.data;
            mesh.vertices = bgfx::createVertexBuffer(copyOf(vertices), meshLayout_);
            if (!data.lightmap.empty()) { // checked by the streamer (TerrainTileData)
                std::string error;
                mesh.lightmap = createGpuTexture(data.lightmap, false, false, BGFX_SAMPLER_UVW_CLAMP, error);
                mesh.lightmapDirection = createGpuTexture(data.lightmapDirection, false, false, BGFX_SAMPLER_UVW_CLAMP, error);
            }
            const bx::Vec3 origin{float(data.tile.x) * map::kTileSize, 0.0f, float(data.tile.z) * map::kTileSize};
            mesh.bounds = {{origin.x, lo, origin.z}, {origin.x + map::kTileSize, hi, origin.z + map::kTileSize}};
        }
        DrawItem item;
        bx::mtxTranslate(item.world.data(), mesh.bounds.min.x, 0.0f, mesh.bounds.min.z);
        item.bounds = mesh.bounds;
        item.material = !info.material.empty() && info.material == previewMaterialPath_ ? &previewMaterial_
                                                                                      : &materials_.get(info.material);
        item.vertices = mesh.vertices;
        item.indices = terrainIndices_;
        if (bgfx::isValid(mesh.lightmap) && bgfx::isValid(mesh.lightmapDirection)) {
            item.lightmap = mesh.lightmap;
            item.lightmapDirection = mesh.lightmapDirection;
        } else if (!data.lightmapError.empty()) {
            warnOnce(std::format("Terrain tile {}_{}: lightmap: {}; lit as unbaked", data.tile.x, data.tile.z,
                                 data.lightmapError));
        }
        draws.push_back(item);
    }
}

void Renderer::pruneTerrain()
{
    for (auto it = terrain_.begin(); it != terrain_.end();) {
        if (std::exchange(it->second.used, false)) {
            ++it;
            continue;
        }
        bgfx::destroy(it->second.vertices);
        for (bgfx::TextureHandle texture : {it->second.lightmap, it->second.lightmapDirection})
            if (bgfx::isValid(texture))
                bgfx::destroy(texture);
        it = terrain_.erase(it);
    }
}

void Renderer::clearTerrain()
{
    for (auto& [data, mesh] : terrain_) {
        bgfx::destroy(mesh.vertices);
        for (bgfx::TextureHandle texture : {mesh.lightmap, mesh.lightmapDirection})
            if (bgfx::isValid(texture))
                bgfx::destroy(texture);
    }
    terrain_.clear();
}

void Renderer::setProjectRoot(const std::filesystem::path& root)
{
    if (root != root_) {
        root_ = root;
        clearTextures();
        clearModels();
        clearProbes();
        voxelGi_->clear();
    }
    materials_.setRoot(root);
    materials_.reloadChanged();
    models().setRoot(root);
    models().reloadChanged();
    shaders().setRoot(root);
    shaders().reloadChanged();
    // ponytail: one stat per texture per frame, as for materials; watch the folder if projects grow to thousands.
    for (auto& [key, texture] : textures_) {
        if (!root_.empty() && modifiedTime(root_ / textureFile(key.first)) != texture.mtime) {
            loadTexture(key.first, key.second, texture);
        } else if (texture.cooking.valid() &&
                   texture.cooking.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            // The cook started on a cache miss is in: its BCn texture replaces the RGBA8 one drawn meanwhile.
            const bgfx::TextureHandle cooked = cookedTexture(key.first, key.second, texture.cooking.get());
            texture.cooking = {};
            if (bgfx::isValid(cooked)) {
                if (bgfx::isValid(texture.handle))
                    bgfx::destroy(texture.handle);
                texture.handle = cooked;
            }
        }
    }
}

bgfx::TextureHandle Renderer::texture(const std::string& path, TextureUsage usage) { return texture(path, usage, white_); }

bgfx::TextureHandle Renderer::texture(const std::string& path, TextureUsage usage, bgfx::TextureHandle fallback)
{
    if (path.empty())
        return fallback;
    if (usage == TextureUsage::Color && path == previewTexturePath_ && bgfx::isValid(previewTexture_))
        return previewTexture_;
    auto [it, added] = textures_.try_emplace({path, usage});
    if (added)
        loadTexture(path, usage, it->second);
    return bgfx::isValid(it->second.handle) ? it->second.handle : fallback;
}

bgfx::TextureHandle Renderer::cookedTexture(const std::string& path, TextureUsage usage, const CookedTexture& cooked)
{
    std::string error = cooked.error;
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    if (!cooked.ktx.empty())
        handle = createGpuTexture(cooked.ktx, isSrgb(usage), false, BGFX_TEXTURE_NONE, error);
    if (!bgfx::isValid(handle))
        warnOnce(std::format("Texture '{}': cannot compress it ({}); drawing it uncompressed", path, error));
    else
        log::info(std::format("Loaded texture '{}' as {} {}", path, usageName(usage),
                              cooked.cached ? "(cache hit)" : std::format("(cooked in {:.2f} s)", cooked.seconds)));
    return handle;
}

bgfx::TextureHandle Renderer::pakTexture(const std::string& path, TextureUsage usage)
{
    // The asked-for usage first; then another's (ponytail: Lua can sample an image as no material does, so Build
    // didn't cook that usage; the other cook's mips or sRGB filtering are slightly off, which beats nothing).
    std::vector<TextureUsage> order{usage};
    for (const TextureUsage other : kTextureUsages)
        if (other != usage)
            order.push_back(other);
    for (const TextureUsage use : order) {
        std::string ktx, error;
        const std::optional<std::filesystem::path> file = resolveProjectPath(root_, texturePakPath(path, use), error);
        if (!file || !readFile(*file, ktx))
            continue;
        const bgfx::TextureHandle handle = createGpuTexture(ktx, isSrgb(usage), false, BGFX_TEXTURE_NONE, error);
        if (bgfx::isValid(handle))
            log::info(std::format("Loaded texture '{}' as {} (cooked)", path, usageName(use)));
        else
            log::error(std::format("Texture '{}': cooked texture '{}': {}; drawing without it", path,
                                   texturePakPath(path, use), error));
        return handle;
    }
    // Logged once: the entry keeps its (pak: unchanging) modification time, so it isn't loaded again.
    log::error(std::format("Texture '{}': game.pak has no cooked texture for it ('{}'; the editor's Build cooks the "
                           "images that materials, models, particles and skies use); drawing without it",
                           path, texturePakPath(path, usage)));
    return BGFX_INVALID_HANDLE;
}

void Renderer::loadTexture(const std::string& path, TextureUsage usage, Texture& texture)
{
    const LoadTimer timer(assetLoadStats().textures, assetLoadStats().textureSeconds);
    if (bgfx::isValid(texture.handle))
        bgfx::destroy(texture.handle);
    texture.handle = BGFX_INVALID_HANDLE;
    texture.cooking = {};
    const bool srgb = isSrgb(usage);
    std::string error = "no project";
    const std::optional<std::filesystem::path> file =
        root_.empty() ? std::nullopt : resolveProjectPath(root_, textureFile(path), error);
    texture.mtime = file ? modifiedTime(*file) : std::nullopt;
    if (pakMounted() && !root_.empty() && !isGpuImage({}, path)) { // the player: Build cooked it (KTX / DDS ship as is)
        texture.handle = pakTexture(path, usage);
        return;
    }
    // An image's bytes: a project file's, or embedded in a model.
    const auto readImage = [&](const std::string& image, std::string& out) {
        if (!embeddedTextureModel(image).empty()) {
            const std::optional<std::string_view> embedded = models().embeddedTexture(image);
            if (embedded)
                out = *embedded;
            else
                error = "no such embedded image";
            return embedded.has_value();
        }
        const std::optional<std::filesystem::path> at =
            root_.empty() ? std::nullopt : resolveProjectPath(root_, image, error);
        const bool ok = at && readFile(*at, out);
        if (at && !ok)
            error = "cannot read the file";
        return ok;
    };
    std::string bytes;
    bool read = false;
    if (const auto orm = ormSources(path)) { // two images packed into one (ormImage), cooked as one
        std::string occlusion;
        std::optional<std::string> packed;
        if (!readImage(orm->first, bytes) || !readImage(orm->second, occlusion))
            error = std::format("'{}': {}", bytes.empty() ? orm->first : orm->second, error);
        else if ((packed = ormImage(bytes, occlusion, error)))
            bytes = std::move(*packed);
        read = packed.has_value();
    } else {
        read = readImage(path, bytes);
    }
    if (read && isGpuImage(bytes, path)) {
        texture.handle = createGpuTexture(bytes, srgb, false, BGFX_TEXTURE_NONE, error);
    } else if (read) {
        if (textureCooks().enabled()) { // the editor: a hit (or a cook not async) is ready now, a miss cooks meanwhile
            std::shared_future<CookedTexture> cook = textureCooks().cook(root_, bytes, usage);
            if (cook.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                texture.cooking = std::move(cook);
            else if (bgfx::isValid(texture.handle = cookedTexture(path, usage, cook.get())))
                return;
        }
        if (const std::optional<MipImage> image = decodeImage(bytes, srgb))
            texture.handle = createTexture(*image, srgb);
        else
            error = std::format("cannot decode it as PNG or JPG ({})", stbi_failure_reason());
    }
    if (!bgfx::isValid(texture.handle))
        warnOnce(std::format("Texture '{}': {}; drawing without it", path, error));
}

void Renderer::setPreview(const std::string& materialPath, const Material& material, const std::string& texturePath,
                          MipImage image)
{
    previewMaterialPath_ = materialPath;
    previewMaterial_ = material;
    previewTexturePath_ = texturePath;
    if (bgfx::isValid(previewTexture_))
        bgfx::destroy(previewTexture_);
    previewTexture_ = BGFX_INVALID_HANDLE;
    if (!materialPath.empty() && !image.rgba.empty())
        previewTexture_ = createTexture(image, true);
}

void Renderer::clearTextures()
{
    for (auto& [key, texture] : textures_)
        if (bgfx::isValid(texture.handle))
            bgfx::destroy(texture.handle);
    textures_.clear();
}

void Renderer::clearModels()
{
    for (auto& [path, gpu] : models_)
        for (ModelMeshBuffers& mesh : gpu.meshes) {
            bgfx::destroy(mesh.vertices);
            bgfx::destroy(mesh.indices);
        }
    models_.clear();
}

const Renderer::GpuModel* Renderer::gpuModel(const std::string& path)
{
    const std::shared_ptr<const Model> model = models().get(path);
    auto it = models_.find(path);
    if (it != models_.end() && it->second.source == model)
        return model ? &it->second : nullptr;
    if (it != models_.end()) { // reloaded or now failing: drop the old buffers
        for (ModelMeshBuffers& mesh : it->second.meshes) {
            bgfx::destroy(mesh.vertices);
            bgfx::destroy(mesh.indices);
        }
        models_.erase(it);
    }
    if (!model)
        return nullptr;
    GpuModel& gpu = models_[path];
    gpu.source = model;
    for (const ModelMesh& mesh : model->meshes)
        gpu.meshes.push_back({bgfx::createVertexBuffer(copyOf(mesh.vertices), modelLayout_),
                              bgfx::createIndexBuffer(copyOf(mesh.indices), BGFX_BUFFER_INDEX32)});
    nodeGlobals(*model, bindPose(*model), gpu.bindGlobals);
    if (model->overBoneLimit) // once per load: the GPU copy is made once
        log::error(std::format("Model '{}' has a mesh with more than {} joints; rendering it in bind pose, unskinned",
                               path, kMaxBones));
    return &gpu;
}

const Renderer::UserShader* Renderer::userShader(const std::string& path)
{
    const std::shared_ptr<const CompiledShader> compiled = shaders().get(path);
    auto it = userShaders_.find(path);
    if (it != userShaders_.end() && it->second.source == compiled)
        return compiled ? &it->second : nullptr;
    if (it != userShaders_.end()) { // recompiled or now failing: drop the old programs
        destroy(it->second);
        userShaders_.erase(it);
    }
    if (!compiled)
        return nullptr;
    UserShader& shader = userShaders_[path];
    shader.source = compiled;
    // A stage it has no bytecode for (no vertex()) takes the built-in shader.
    const auto make = [&](ShaderStage stage, bgfx::ShaderHandle builtin) {
        const std::vector<uint8_t>& bytecode = compiled->stage(stage);
        if (bytecode.empty())
            return builtin;
        shader.shaders.push_back(
            bgfx::createShader(bgfx::copy(bytecode.data(), static_cast<uint32_t>(bytecode.size()))));
        return shader.shaders.back();
    };
    const bgfx::ShaderHandle fragment = make(ShaderStage::Fragment, BGFX_INVALID_HANDLE);
    const bgfx::ShaderHandle shadow = make(ShaderStage::Shadow, BGFX_INVALID_HANDLE);
    const bgfx::ShaderHandle vertex = make(ShaderStage::Vertex, meshVertex_);
    const bgfx::ShaderHandle skinned = make(ShaderStage::SkinnedVertex, skinnedVertex_);
    const bgfx::ShaderHandle instanced = make(ShaderStage::InstancedVertex, instancedVertex_);
    shader.mesh = bgfx::createProgram(vertex, fragment);
    shader.skinned = bgfx::createProgram(skinned, fragment);
    shader.instanced = bgfx::createProgram(instanced, fragment);
    shader.shadowMesh = bgfx::createProgram(vertex, shadow);
    shader.shadowSkinned = bgfx::createProgram(skinned, shadow);
    shader.shadowInstanced = bgfx::createProgram(instanced, shadow);
    return &shader;
}

void Renderer::destroy(UserShader& shader)
{
    for (bgfx::ProgramHandle program : {shader.mesh, shader.skinned, shader.instanced, shader.shadowMesh,
                                        shader.shadowSkinned, shader.shadowInstanced})
        bgfx::destroy(program);
    for (bgfx::ShaderHandle handle : shader.shaders)
        bgfx::destroy(handle);
}

void Renderer::resolveShaders(std::vector<DrawItem>& draws)
{
    // Once per distinct material; a null material: its shader is still compiling for the first time, so it isn't drawn.
    std::map<const Material*, std::pair<const Material*, const UserShader*>> resolved;
    for (DrawItem& item : draws) {
        if (item.material->shader.empty())
            continue;
        auto [it, added] = resolved.try_emplace(item.material, item.material, nullptr);
        if (added) {
            const Material& material = *item.material;
            const UserShader* shader = userShader(material.shader); // null: the library logged why, or compiling
            const bool compiling = !shader && shaders().status(material.shader).state == ShaderLibrary::State::Compiling;
            std::string error;
            std::vector<std::string> unknown;
            if (shader && (!checkShaderParams(material, shader->source->source, error, unknown) ||
                           !checkShaderAlphaMode(material, shader->source->source, error))) {
                warnOnce(std::format("Shader '{}': {}; rendering the material magenta", material.shader, error));
                shader = nullptr;
            }
            for (const std::string& name : unknown)
                warnOnce(std::format("Shader '{}' declares no param '{}'; ignoring the material's value", material.shader,
                                     name));
            it->second = {compiling ? nullptr : shader ? &material : &errorMaterial(), shader};
        }
        item.material = it->second.first;
        item.shader = it->second.second;
    }
    std::erase_if(draws, [](const DrawItem& item) { return !item.material; });
}

void Renderer::addModelDraws(const Scene& scene, entt::entity entity, const ModelRenderer& renderer, const float* world,
                             std::vector<DrawItem>& draws, std::vector<std::vector<Mat4>>& boneSets)
{
    if (renderer.model.empty())
        return;
    DrawItem item;
    std::copy_n(world, 16, item.world.begin());
    item.castShadows = renderer.castShadows;
    item.receiveShadows = renderer.receiveShadows;
    const GpuModel* gpu = gpuModel(renderer.model);
    if (!gpu) { // missing, corrupt or unsupported: the library warned once
        const Mesh& cube = meshes_[static_cast<size_t>(Primitive::Cube)];
        item.bounds = transformBounds(primitiveBounds(Primitive::Cube), world);
        item.material = &errorMaterial();
        item.vertices = cube.vertices;
        item.indices = cube.indices;
        draws.push_back(item);
        return;
    }
    const Model& model = *gpu->source;
    const auto* state = scene.registry().try_get<AnimatorState>(entity);
    const std::vector<Mat4>& globals =
        state && state->model == &model && state->globals.size() == model.nodes.size() ? state->globals : gpu->bindGlobals;
    item.bounds = transformBounds(model.bounds, world); // ponytail: bind-pose bounds, also for posed models
    item.posed = &globals != &gpu->bindGlobals;
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const ModelMesh& mesh = model.meshes[i];
        const std::string* slot = mesh.material < renderer.materials.size() ? &renderer.materials[mesh.material] : nullptr;
        item.material = slot && !slot->empty() ? &materials_.get(*slot) : &model.materials[mesh.material].material;
        item.vertices = gpu->meshes[i].vertices;
        item.indices = gpu->meshes[i].indices;
        item.bones = -1;
        if (!mesh.joints.empty() && mesh.joints.size() <= kMaxBones) {
            std::copy_n(world, 16, item.world.begin());
            boneSets.emplace_back();
            skinMatrices(mesh, globals, boneSets.back());
            item.bones = static_cast<int>(boneSets.size() - 1);
        } else { // static, or over the bone limit: at its node
            const std::vector<Mat4>& at = mesh.joints.empty() ? globals : gpu->bindGlobals;
            bx::mtxMul(item.world.data(), at[static_cast<size_t>(mesh.node)].data(), world);
        }
        item.mirrored = isMirrored(item.world); // ponytail: skinned meshes by their entity matrix; mirrored joints aren't seen
        draws.push_back(item);
    }
}

bool Renderer::instanced(const std::vector<const DrawItem*>& batch, bgfx::InstanceDataBuffer& instances) const
{
    // i_data0..3 in vs_mesh_instanced.sc and vs_shadow_instanced.sc; a lightmapped batch's i_data4 after them (the
    // batch shares its lightmap: BatchKey).
    const bool lightmapped = bgfx::isValid(batch.front()->lightmap);
    const auto stride = static_cast<uint16_t>(sizeof(Mat4) + (lightmapped ? 4 * sizeof(float) : 0));
    const auto count = static_cast<uint32_t>(batch.size());
    // ponytail: a batch this frame's transient buffer can't hold whole draws one by one; split it if that shows.
    if (!instancing_ || count < 2 || batch.front()->bones >= 0 || !(bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING) ||
        bgfx::getAvailInstanceDataBuffer(count, stride) < count)
        return false;
    bgfx::allocInstanceDataBuffer(&instances, count, stride);
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(instances.data + i * stride, batch[i]->world.data(), sizeof(Mat4));
        if (lightmapped)
            std::memcpy(instances.data + i * stride + sizeof(Mat4), batch[i]->lightmapST.data(), 4 * sizeof(float));
    }
    return true;
}

void Renderer::submitShadowCasters(const std::vector<const DrawItem*>& batch, const std::vector<std::vector<Mat4>>& boneSets,
                                   const bx::Vec3& eye, bgfx::ViewId view)
{
    // The batch shares its material, buffers and skinning (BatchKey).
    const DrawItem& first = *batch.front();
    const Material& material = *first.material;
    const UserShader* shader = first.shader;
    const float params[4] = {0.0f, 0.0f, material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : -1.0f, 0.0f};
    const float uvTransform[4] = {material.uvScale[0], material.uvScale[1], material.uvOffset[0], material.uvOffset[1]};
    const float cameraPos[4] = {eye.x, eye.y, eye.z, 1.0f};
    const auto setMaterial = [&] {
        if (shader) { // its vertex() and fragment() run here too: all that fs_mesh.sc reads before its shadow cut
            setMaterialState(first);
            bgfx::setUniform(cameraPos_, cameraPos);
        } else {
            bgfx::setUniform(baseColor_, material.baseColor.data());
            bgfx::setUniform(materialParams_, params);
            bgfx::setUniform(uvTransform_, uvTransform);
            bgfx::setTexture(0, baseColorSampler_, texture(material.baseColorTexture, TextureUsage::Color));
        }
        bgfx::setVertexBuffer(0, first.vertices);
        bgfx::setIndexBuffer(first.indices);
        bgfx::setState(kShadowState);
    };
    bgfx::InstanceDataBuffer instances;
    if (instanced(batch, instances)) {
        setMaterial();
        bgfx::setInstanceDataBuffer(&instances);
        bgfx::submit(view, shader ? shader->shadowInstanced : shadowInstancedProgram_);
        return;
    }
    for (const DrawItem* item : batch) {
        setMaterial();
        bgfx::setTransform(item->world.data());
        if (shader) {
            float normal[16];
            normalMatrix(item->world, normal);
            bgfx::setUniform(normalMatrix_, normal);
        }
        if (item->bones >= 0) {
            const std::vector<Mat4>& bones = boneSets[static_cast<size_t>(item->bones)];
            bgfx::setUniform(bones_, bones.data(), static_cast<uint16_t>(bones.size()));
        }
        if (shader)
            bgfx::submit(view, item->bones >= 0 ? shader->shadowSkinned : shader->shadowMesh);
        else
            bgfx::submit(view, item->bones >= 0 ? shadowSkinnedProgram_ : shadowProgram_);
    }
}

Renderer::ShadowFrame Renderer::renderShadows(const DirectionalLight* sun, const bx::Vec3& sunDir,
                                              const std::vector<DrawItem>& draws,
                                              const std::vector<std::vector<Mat4>>& boneSets,
                                              const std::vector<LocalLightSample>& lights, const float* viewMtx,
                                              const float* projMtx, bgfx::ViewId shadowViews)
{
    ShadowFrame frame;
    std::vector<const DrawItem*> casters; // blended and additive materials never cast
    for (const DrawItem& item : draws)
        if (item.castShadows && !isTransparent(*item.material))
            casters.push_back(&item);
    // Batched once; each view then draws the casters of each batch inside its frustum (and that keep accepts).
    std::vector<std::vector<const DrawItem*>> batches;
    for (const std::vector<size_t>& indices :
         batchDraws(casters.size(), [&](size_t i) { return batchKey(*casters[i], {}); })) {
        batches.emplace_back();
        for (size_t i : indices)
            batches.back().push_back(casters[i]);
    }
    float cameraWorld[16];
    bx::mtxInverse(cameraWorld, viewMtx);
    const bx::Vec3 eye{cameraWorld[12], cameraWorld[13], cameraWorld[14]};
    std::vector<const DrawItem*> inView;
    const auto submitCasters = [&](const Frustum& frustum, const auto& keep, bgfx::ViewId view) {
        for (const std::vector<const DrawItem*>& batch : batches) {
            inView.clear();
            for (const DrawItem* item : batch)
                if (keep(*item) && !culled(*item, frustum))
                    inView.push_back(item);
            if (!inView.empty())
                submitShadowCasters(inView, boneSets, eye, view);
        }
    };
    // Casters whose bounds are within the light's range of it.
    const auto inRangeOf = [](const LocalLightSample& light) {
        return [&light](const DrawItem& item) {
            const bx::Vec3 closest = bx::clamp(light.position, item.bounds.min, item.bounds.max);
            return bx::length(bx::sub(light.position, closest)) < light.range;
        };
    };
    const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;
    // One view into atlas tile (column, row) (see shadow.h), and its world to (atlas u, v (down), 0..1 depth) as out.
    const auto atlasView = [&](bgfx::ViewId view, const char* name, int column, int row, const float* lightView,
                               const float* lightProj, float* out, const auto& keep) {
        bgfx::setViewName(view, name);
        bgfx::setViewFrameBuffer(view, csmFrameBuffer_);
        bgfx::setViewRect(view, static_cast<uint16_t>(column * kCascadeSize), static_cast<uint16_t>(row * kCascadeSize),
                          kCascadeSize, kCascadeSize);
        bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
        bgfx::setViewTransform(view, lightView, lightProj);
        bgfx::touch(view);
        float viewProj[16];
        bx::mtxMul(viewProj, lightView, lightProj);
        submitCasters(frustumFromViewProj(viewProj, homogeneousDepth), keep, view);
        // NDC to the tile: half a tile's share of the atlas each way from its center.
        constexpr float kHalfU = 0.5f * kCascadeSize / kShadowAtlasWidth;
        constexpr float kHalfV = 0.5f * kCascadeSize / kShadowAtlasHeight;
        float tile[16];
        bx::mtxIdentity(tile);
        tile[0] = kHalfU;
        tile[5] = -kHalfV;
        tile[12] = kHalfU * static_cast<float>(1 + 2 * column);
        tile[13] = kHalfV * static_cast<float>(1 + 2 * row);
        if (homogeneousDepth) { // -1..1 to the 0..1 a depth texture holds
            tile[10] = 0.5f;
            tile[14] = 0.5f;
        }
        bx::mtxMul(out, viewProj, tile);
    };

    frame.sun = sun && sun->castShadows;
    frame.tileLights = shadowedLights(lights, eye, false);
    if ((frame.sun || !frame.tileLights.empty()) && !bgfx::isValid(csm_)) {
        csm_ = bgfx::createTexture2D(kShadowAtlasWidth, kShadowAtlasHeight, false, 1, bgfx::TextureFormat::D16,
                                     kShadowMapFlags);
        csmFrameBuffer_ = bgfx::createFrameBuffer(1, &csm_, false);
    }
    if (frame.sun) {
        // The camera's near and far planes, as view depths.
        float invProj[16];
        bx::mtxInverse(invProj, projMtx);
        const float cameraNear = -bx::mulH({0.0f, 0.0f, homogeneousDepth ? -1.0f : 0.0f}, invProj).z;
        const float cameraFar = -bx::mulH({0.0f, 0.0f, 1.0f}, invProj).z;
        const int count = std::clamp(sun->cascadeCount, 1, kMaxShadowCascades);
        const auto splits = cascadeSplits(cameraNear, std::min(sun->maxDistance, cameraFar), count);
        float casterNear = FLT_MAX;
        for (const DrawItem* item : casters)
            for (int corner = 0; corner < 8; ++corner) {
                const bx::Vec3 p{corner & 1 ? item->bounds.max.x : item->bounds.min.x,
                                 corner & 2 ? item->bounds.max.y : item->bounds.min.y,
                                 corner & 4 ? item->bounds.max.z : item->bounds.min.z};
                casterNear = std::min(casterNear, bx::dot(p, sunDir));
            }
        for (int c = 0; c < kMaxShadowCascades; ++c) {
            const auto i = static_cast<size_t>(c);
            frame.csmSplits[i] = splits[i + 1];
            if (c >= count)
                continue;
            const Cascade cascade = fitCascade(frustumSliceCorners(viewMtx, projMtx, splits[i], splits[i + 1]), sunDir,
                                               casterNear, homogeneousDepth);
            // The ortho volume reaches back to the nearest caster (fitCascade), so casters between the light and the
            // camera's slice stay in it.
            atlasView(static_cast<bgfx::ViewId>(shadowViews + c), "Shadow cascade", c % 2, c / 2, cascade.view.data(),
                      cascade.proj.data(), frame.csmMtx[i], [](const DrawItem&) { return true; });
            frame.csmBias[i][0] = sun->normalBias * cascade.texelSize;
            frame.csmBias[i][1] = sun->bias / cascade.depthRange;
        }
    }

    // Spot and area lights: a perspective view from the light (the rect's center) along its direction, as wide as the
    // spot's outer cone or kAreaShadowFovDegrees, out to its range.
    for (size_t t = 0; t < frame.tileLights.size(); ++t) {
        const LocalLightSample& light = lights[frame.tileLights[t]];
        const bx::Vec3 up = std::abs(light.direction.y) > 0.99f ? bx::Vec3{0.0f, 0.0f, 1.0f} : bx::Vec3{0.0f, 1.0f, 0.0f};
        const float fov = light.type == LocalLightType::Spot ? 2.0f * bx::toDeg(std::acos(light.cosOuter))
                                                             : kAreaShadowFovDegrees;
        float lightView[16], lightProj[16];
        bx::mtxLookAt(lightView, light.position, bx::add(light.position, light.direction), up, bx::Handedness::Right);
        bx::mtxProj(lightProj, fov, 1.0f, kPointShadowNear, light.range, homogeneousDepth, bx::Handedness::Right);
        const auto tile = static_cast<int>(t);
        atlasView(static_cast<bgfx::ViewId>(shadowViews + kMaxShadowCascades + kMaxShadowedPointLights * 6 + t),
                  "Shadow spot / area tile", tile % 2, 2 + tile / 2, lightView, lightProj,
                  frame.csmMtx[kMaxShadowCascades + t], inRangeOf(light));
    }

    frame.pointLights = shadowedLights(lights, eye, true);
    for (size_t cube = 0; cube < frame.pointLights.size(); ++cube) {
        if (!bgfx::isValid(cubes_[cube])) {
            cubes_[cube] = bgfx::createTextureCube(kPointShadowSize, false, 1, bgfx::TextureFormat::D32F, kShadowMapFlags);
            for (uint16_t face = 0; face < 6; ++face) {
                bgfx::Attachment attachment;
                attachment.init(cubes_[cube], bgfx::Access::Write, face);
                cubeFrameBuffers_[cube][face] = bgfx::createFrameBuffer(1, &attachment, false);
            }
        }
        const LocalLightSample& light = lights[frame.pointLights[cube]];
        const auto inRange = inRangeOf(light);
        // ponytail: D3D 0..1 depth assumed (fs_mesh.sc pointShadow), as for the DX11-only shaders.
        float proj[16];
        bx::mtxProj(proj, 90.0f, 1.0f, kPointShadowNear, light.range, false, bx::Handedness::Left);
        for (uint16_t face = 0; face < 6; ++face) {
            const auto view = static_cast<bgfx::ViewId>(shadowViews + kMaxShadowCascades + cube * 6 + face);
            float faceView[16];
            bx::mtxLookAt(faceView, light.position, bx::add(light.position, kCubeFaceForward[face]), kCubeFaceUp[face],
                          bx::Handedness::Left);
            bgfx::setViewName(view, "Shadow cube face");
            bgfx::setViewFrameBuffer(view, cubeFrameBuffers_[cube][face]);
            bgfx::setViewRect(view, 0, 0, kPointShadowSize, kPointShadowSize);
            bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
            bgfx::setViewTransform(view, faceView, proj);
            bgfx::touch(view);
            float viewProj[16];
            bx::mtxMul(viewProj, faceView, proj);
            submitCasters(frustumFromViewProj(viewProj, false), inRange, view);
        }
    }
    return frame;
}

void Renderer::setMaterialState(const DrawItem& item)
{
    const Material& material = *item.material;
    const bx::Vec3 radiance = emissiveRadiance(material);
    const float emissive[4] = {radiance.x, radiance.y, radiance.z, 0.0f};
    const bool blend = material.alphaMode == AlphaMode::Blend;
    const bool additive = material.alphaMode == AlphaMode::Additive;
    // z: fragments with alpha below it are discarded (-1 = none); w: 1 outputs alpha (blend), 2 also fogs toward
    // black (additive), 0 outputs 1.
    const float params[4] = {material.metallic, material.roughness,
                             material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : -1.0f,
                             additive ? 2.0f : blend ? 1.0f : 0.0f};
    const bgfx::TextureHandle normalMap = texture(material.normalTexture, TextureUsage::Normal, flatNormal_);
    const float materialExt[4] = {material.normalScale, material.occlusionStrength,
                                  normalMap.idx != flatNormal_.idx ? 1.0f : 0.0f, material.unlit ? 1.0f : 0.0f};
    const float uvTransform[4] = {material.uvScale[0], material.uvScale[1], material.uvOffset[0], material.uvOffset[1]};
    const float time[4] = {timeSeconds_, 0.0f, 0.0f, 0.0f};

    bgfx::setUniform(baseColor_, material.baseColor.data());
    bgfx::setUniform(emissive_, emissive);
    bgfx::setUniform(materialParams_, params);
    bgfx::setUniform(materialExt_, materialExt);
    bgfx::setUniform(uvTransform_, uvTransform);
    bgfx::setUniform(time_, time);
    bgfx::setTexture(0, baseColorSampler_, texture(material.baseColorTexture, TextureUsage::Color));
    // Stage 1 holds metallic-roughness (G, B) and occlusion (R), u_materialOrm says which: one image with both, either
    // alone, or the two packed into one (a pair that fails to pack: the metallic-roughness image, without occlusion).
    const std::string& mr = material.metallicRoughnessTexture;
    const std::string& ao = material.occlusionTexture;
    bgfx::TextureHandle orm = texture(ormTexturePath(material), TextureUsage::Linear, BGFX_INVALID_HANDLE);
    const bool occlusion = !ao.empty() && (bgfx::isValid(orm) || mr.empty() || mr == ao);
    if (!bgfx::isValid(orm))
        orm = texture(mr.empty() ? ao : mr, TextureUsage::Linear);
    const float ormChannels[4] = {mr.empty() ? 0.0f : 1.0f, occlusion ? 1.0f : 0.0f, 0.0f, 0.0f};
    bgfx::setUniform(materialOrm_, ormChannels);
    bgfx::setTexture(1, metallicRoughnessSampler_, orm);
    bgfx::setTexture(7, normalSampler_, normalMap);
    bgfx::setTexture(9, emissiveSampler_, texture(material.emissiveTexture, TextureUsage::Color));
    if (item.shader) {
        const ShaderSource& source = item.shader->source->source;
        const auto values = shaderParamValues(material, source);
        bgfx::setUniform(kastParams_, values.data(), kMaxShaderParams);
        for (const ShaderParam& param : source.params) {
            if (param.type != ShaderParamType::Texture2D)
                continue;
            const bgfx::TextureHandle fallback = param.texture == TextureDefault::Black    ? black_
                                                 : param.texture == TextureDefault::Normal ? flatNormal_
                                                                                           : white_;
            bgfx::setTexture(static_cast<uint8_t>(kFirstShaderTextureStage + param.slot),
                             shaderTextureSamplers_[static_cast<size_t>(param.slot)],
                             texture(shaderParamTexture(material, param), colorUsage(param.srgb), fallback));
        }
    }
}

void Renderer::updateSkyEnvironment(const SceneSky& sky, bgfx::ViewId view)
{
    const bool cube = sky.mode == SkyMode::Cubemap && bgfx::isValid(sky_.cube()); // else the gradient (a fallback too)
    const SkyEnvKey key{sky.mode, cube, cube ? sky_.cubeVersion() : 0,
                        {sky.top.x, sky.top.y, sky.top.z, sky.horizon.x, sky.horizon.y, sky.horizon.z, sky.bottom.x,
                         sky.bottom.y, sky.bottom.z}};
    if (skyEnvKey_ == key)
        return;
    skyEnvKey_ = key;
    if (bgfx::isValid(skyEnv_))
        bgfx::destroy(skyEnv_);
    skyEnv_ = BGFX_INVALID_HANDLE;
    if (sky.mode == SkyMode::Off)
        return;
    skyEnv_ = envFilter_.sky(view, sky, cube ? sky_.cube() : bgfx::TextureHandle{bgfx::kInvalidHandle}, sky_.cubeSize());
    skySh_ = cube ? sky_.cubeSh() : proceduralSkySh9(sky);
    if (!envFilter_.supported())
        warnOnce("Sky lighting: the GPU has no compute shader support; meshes get the flat ambient");
}

const Renderer::ProbeFile& Renderer::probeFile(const ReflectionProbe& probe)
{
    std::string error = "no project";
    const std::optional<std::filesystem::path> ktx =
        root_.empty() ? std::nullopt : resolveProjectPath(root_, probe.bakedPath, error);
    const auto ktxTime = ktx ? modifiedTime(*ktx) : std::nullopt;
    const auto jsonTime = ktx ? modifiedTime(probeShPath(*ktx)) : std::nullopt;
    auto [it, added] = probeFiles_.try_emplace({probe.bakedPath, probe.resolution});
    ProbeFile& file = it->second;
    if (!added && file.ktxTime == ktxTime && file.jsonTime == jsonTime) // a re-bake rewrites both
        return file;
    const LoadTimer timer(assetLoadStats().textures, assetLoadStats().textureSeconds);
    if (bgfx::isValid(file.cube))
        bgfx::destroy(file.cube);
    file = {.ktxTime = ktxTime, .jsonTime = jsonTime};
    std::string bytes, json;
    if (ktx && !readFile(*ktx, bytes))
        error = "cannot read the file";
    else if (ktx && !readFile(probeShPath(*ktx), json))
        error = "cannot read its .json (spherical harmonics)";
    else if (ktx && checkBakedProbe(bytes, json, static_cast<uint16_t>(probe.resolution), file.sh, error))
        file.cube = createGpuTexture(bytes, false, true, BGFX_SAMPLER_UVW_CLAMP, error);
    if (!bgfx::isValid(file.cube))
        file.error = error;
    else
        log::info(std::format("Loaded reflection probe '{}'", probe.bakedPath));
    return file;
}

void Renderer::clearProbes()
{
    for (auto& [key, file] : probeFiles_)
        if (bgfx::isValid(file.cube))
            bgfx::destroy(file.cube);
    probeFiles_.clear();
}

std::string Renderer::probeStatus(const ReflectionProbe& probe)
{
    if (probe.bakedPath.empty())
        return "Not baked";
    const ProbeFile& file = probeFile(probe);
    return file.error.empty() ? "Baked" : file.error;
}

void Renderer::clearLightmaps()
{
    for (const std::vector<bgfx::TextureHandle>* textures : {&lightmaps_.irradiance, &lightmaps_.direction})
        for (bgfx::TextureHandle texture : *textures)
            bgfx::destroy(texture);
    for (auto& [key, mesh] : lightmaps_.buffers) {
        bgfx::destroy(mesh.vertices);
        bgfx::destroy(mesh.indices);
    }
    lightmaps_ = {};
}

Renderer::LightmapSet& Renderer::lightmapSet(const std::string& bakedPath)
{
    std::string error = "no project";
    const std::optional<std::filesystem::path> json =
        root_.empty() ? std::nullopt : resolveProjectPath(root_, bakedPath, error);
    const auto jsonTime = json ? modifiedTime(*json) : std::nullopt;
    if (lightmaps_.path == bakedPath && lightmaps_.jsonTime == jsonTime) // a re-bake rewrites the json last
        return lightmaps_;
    const LoadTimer timer(assetLoadStats().textures, assetLoadStats().textureSeconds);
    clearLightmaps();
    LightmapSet& set = lightmaps_;
    set.path = bakedPath;
    set.jsonTime = jsonTime;
    const std::filesystem::path folder = json ? json->parent_path() : std::filesystem::path();
    std::string text, bytes;
    const auto load = [&](const std::string& name, uint64_t hash, std::string& out) {
        if (!readFile(folder / std::filesystem::path(reinterpret_cast<const char8_t*>(name.c_str())), out)) {
            error = std::format("cannot read '{}'", name);
            return false;
        }
        if (lightmapFileHash(out) != hash) {
            error = std::format("'{}' doesn't match lightmaps.json (bake again)", name);
            return false;
        }
        return true;
    };
    bool ok = json && readFile(*json, text);
    if (json && !ok)
        error = "cannot read the file";
    ok = ok && parseLightmapSidecar(text, set.sidecar, error);
    for (size_t i = 0; ok && i < set.sidecar.atlases.size(); ++i) {
        const LightmapAtlas& atlas = set.sidecar.atlases[i];
        for (const auto& [name, hash, textures] :
             {std::tuple(atlas.irradiance, atlas.irradianceHash, &set.irradiance),
              std::tuple(atlas.direction, atlas.directionHash, &set.direction)}) {
            ok = ok && load(name, hash, bytes) && checkLightmapKtx(bytes, atlas.width, atlas.height, error);
            const bgfx::TextureHandle texture =
                ok ? createGpuTexture(bytes, false, false, BGFX_SAMPLER_UVW_CLAMP, error) : bgfx::TextureHandle BGFX_INVALID_HANDLE;
            ok = ok && bgfx::isValid(texture);
            if (ok)
                textures->push_back(texture);
        }
    }
    ok = ok && load(set.sidecar.meshes, set.sidecar.meshesHash, bytes) && readLightmapMeshes(bytes, set.meshes, error);
    if (!ok) {
        const std::string why = error;
        clearLightmaps();
        lightmaps_.path = bakedPath;
        lightmaps_.jsonTime = jsonTime;
        lightmaps_.error = why;
        warnOnce(std::format("Lightmaps '{}': {}; the scene renders unbaked", bakedPath, why));
        return lightmaps_;
    }
    for (size_t i = 0; i < set.meshes.size(); ++i)
        set.meshIndex[set.meshes[i].key] = i;
    for (size_t i = 0; i < set.sidecar.entities.size(); ++i)
        set.entities[set.sidecar.entities[i].id] = i;
    log::info(std::format("Loaded lightmaps '{}' ({} atlas pages, {} meshes, {} probes)", bakedPath,
                          set.sidecar.atlases.size(), set.sidecar.entities.size(), set.sidecar.grid.sh.size()));
    return set;
}

bool Renderer::applyLightmap(LightmapSet& set, EntityId id, const std::string& source, size_t submesh,
                             DrawItem& item)
{
    const auto entity = set.entities.find(id);
    if (!set.error.empty() || entity == set.entities.end())
        return false;
    const LightmapEntity& e = set.sidecar.entities[entity->second];
    const auto mesh = set.meshIndex.find(e.mesh);
    if (mesh == set.meshIndex.end() || set.meshes[mesh->second].source != source ||
        submesh >= set.meshes[mesh->second].submeshes.size())
        return false;
    const std::pair key{mesh->second, submesh};
    if (set.broken.contains(key))
        return false;
    auto buffers = set.buffers.find(key);
    if (buffers == set.buffers.end()) { // the source's vertices, copied out along xref with their lightmap UVs
        const LightmapSubmesh& sub = set.meshes[mesh->second].submeshes[submesh];
        std::vector<MeshVertex> source_vertices;
        if (source.starts_with("primitive:")) {
            const std::string name = source.substr(10);
            const Primitive primitive = name == "plane" ? Primitive::Plane : name == "sphere" ? Primitive::Sphere : Primitive::Cube;
            source_vertices = primitiveMesh(primitive).vertices;
        } else if (const GpuModel* gpu = gpuModel(source.substr(6)); gpu && submesh < gpu->source->meshes.size()) {
            for (const ModelVertex& v : gpu->source->meshes[submesh].vertices)
                source_vertices.push_back({v.x, v.y, v.z, v.nx, v.ny, v.nz, v.tx, v.ty, v.tz, v.tw, v.u, v.v, v.color});
        }
        LightmapSet& owned = set;
        if (source_vertices.size() != sub.sourceVertices) {
            owned.broken.insert(key);
            warnOnce(std::format("Lightmaps '{}': '{}' changed since the bake; its meshes render unbaked (bake again)",
                                 set.path, source));
            return false;
        }
        std::vector<MeshVertex> vertices;
        vertices.reserve(sub.xref.size());
        for (size_t v = 0; v < sub.xref.size(); ++v) {
            MeshVertex vertex = source_vertices[sub.xref[v]];
            vertex.u2 = sub.uv[v * 2];
            vertex.v2 = sub.uv[v * 2 + 1];
            vertices.push_back(vertex);
        }
        buffers = owned.buffers
                      .emplace(key, Mesh{bgfx::createVertexBuffer(copyOf(vertices), meshLayout_),
                                         bgfx::createIndexBuffer(copyOf(sub.indices), BGFX_BUFFER_INDEX32)})
                      .first;
    }
    item.vertices = buffers->second.vertices;
    item.indices = buffers->second.indices;
    item.lightmap = set.irradiance[e.atlas];
    item.lightmapDirection = set.direction[e.atlas];
    item.lightmapST = e.scaleOffset;
    return true;
}

std::string Renderer::lightmapStatus(const Scene& scene)
{
    const std::string& path = scene.lighting.lightmaps.bakedPath;
    if (path.empty())
        return "Not baked";
    const LightmapSet& set = lightmapSet(path);
    if (!set.error.empty())
        return set.error;
    return set.sidecar.hash == lightmapSceneHash(scene) ? "Baked" : "Out of date";
}

std::string Renderer::voxelGiStatus(const Scene& scene, entt::entity volume)
{
    const VoxelGI& component = scene.registry().get<VoxelGI>(volume);
    if (component.bakedPath.empty())
        return "Not baked";
    if (!voxelGi_->passes.supported())
        return "This GPU can't run VoxelGI (no compute or 3D images)";
    if (root_.empty())
        return ""; // not rendered yet: its project's files aren't known
    const VoxelGiState::File& file = voxelGi_->file(root_, component);
    if (!file.error.empty())
        return file.error;
    return file.sidecar.hash == voxelGiSceneHash(scene, volume) ? "Baked" : "Out of date";
}

bool Renderer::bakeProbe(const Scene& scene, const Scene* extra, const std::filesystem::path& projectRoot,
                         const bx::Vec3& center, uint16_t size, bgfx::ViewId firstView, BakedProbe& out,
                         std::string& error)
{
    if (!envFilter_.supported()) {
        error = "the GPU has no compute shader support";
        return false;
    }
    // The six faces, one frame each (a face is a whole render(): its own shadow maps in the views before sceneView).
    const auto sceneView = static_cast<bgfx::ViewId>(firstView + kShadowViews);
    const bgfx::TextureHandle color =
        bgfx::createTextureCube(size, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP);
    const bgfx::TextureHandle depth =
        bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
    const bx::Vec3 background = sceneClearColor(scene.postProcessing, 0x000000ff); // fog, else black
    const auto palette = static_cast<uint8_t>(sceneView % 16); // PostProcess::begin's entry for this view
    bgfx::setPaletteColor(palette, background.x, background.y, background.z, 1.0f);
    float proj[16];
    bx::mtxProj(proj, 90.0f, 1.0f, kProbeNear, kProbeFar, bgfx::getCaps()->homogeneousDepth, bx::Handedness::Right);
    capture_ = true;
    for (uint16_t face = 0; face < 6; ++face) {
        bgfx::Attachment attachments[2];
        attachments[0].init(color, bgfx::Access::Write, face);
        attachments[1].init(depth);
        const bgfx::FrameBufferHandle frameBuffer = bgfx::createFrameBuffer(2, attachments, false);
        bgfx::setViewName(sceneView, "Probe bake");
        bgfx::setViewFrameBuffer(sceneView, frameBuffer);
        bgfx::setViewRect(sceneView, 0, 0, size, size);
        bgfx::setViewClear(sceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 1.0f, 0, palette);
        bgfx::touch(sceneView);
        float viewMtx[16];
        bx::mtxLookAt(viewMtx, center, bx::add(center, kProbeFaceForward[face]), kProbeFaceUp[face], bx::Handedness::Right);
        render(scene, projectRoot, sceneView, viewMtx, proj, firstView, extra);
        bgfx::frame();
        bgfx::destroy(frameBuffer);
    }
    capture_ = false;

    // Prefiltered (compute, in firstView), then each level's six sides blitted below one another into a read-back
    // texture (in the view after) and read back.
    for (bgfx::ViewId view = firstView; view <= sceneView; ++view)
        bgfx::resetView(view); // the faces' and shadow maps' settings: the next frame sets what it uses
    bgfx::setViewMode(firstView, bgfx::ViewMode::Sequential);
    const bgfx::TextureHandle env = envFilter_.prefilter(firstView, color, size);
    const auto blitView = static_cast<bgfx::ViewId>(firstView + 1);
    bgfx::touch(blitView);
    uint8_t levels = 1;
    for (uint16_t s = size; s > 1; s /= 2)
        ++levels;
    std::vector<bgfx::TextureHandle> readback(levels);
    std::vector<std::vector<uint16_t>> halves(levels); // RGBA16F
    uint32_t ready = 0;
    for (uint8_t level = 0; level < levels; ++level) {
        const auto s = static_cast<uint16_t>(size >> level);
        readback[level] = bgfx::createTexture2D(s, static_cast<uint16_t>(s * 6), false, 1, bgfx::TextureFormat::RGBA16F,
                                                BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        for (uint16_t side = 0; side < 6; ++side)
            bgfx::blit(blitView, readback[level], 0, 0, static_cast<uint16_t>(side * s), 0, env, level, 0, 0, side, s, s);
        halves[level].resize(static_cast<size_t>(s) * s * 6 * 4);
        ready = std::max(ready, bgfx::readTexture(readback[level], halves[level].data()));
    }
    while (bgfx::frame() < ready) {
    }
    for (bgfx::TextureHandle texture : readback)
        bgfx::destroy(texture);
    for (bgfx::TextureHandle texture : {env, color, depth})
        bgfx::destroy(texture);

    out = makeBakedProbe(size, halves);
    return true;
}

void Renderer::setTarget(bgfx::FrameBufferHandle target, uint16_t width, uint16_t height)
{
    target_ = target;
    targetWidth_ = width;
    targetHeight_ = height;
}

void Renderer::copyScene(bgfx::ViewId view, bgfx::FrameBufferHandle target, bool screen, bool depth)
{
    if (copyWidth_ != targetWidth_ || copyHeight_ != targetHeight_) {
        destroyCopies();
        copyWidth_ = targetWidth_;
        copyHeight_ = targetHeight_;
    }
    if (screen) {
        if (!bgfx::isValid(screenCopy_))
            screenCopy_ = bgfx::createTexture2D(copyWidth_, copyHeight_, false, 1, bgfx::TextureFormat::RGBA16F,
                                                BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_UVW_CLAMP);
        bgfx::blit(view, screenCopy_, 0, 0, bgfx::getTexture(target, 0));
        ++copies_.screen;
    }
    if (depth) {
        if (!bgfx::isValid(depthCopy_)) // D3D11 copies depth whole: the target's format and size
            depthCopy_ = bgfx::createTexture2D(copyWidth_, copyHeight_, false, 1, bgfx::TextureFormat::D24S8,
                                               BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
        bgfx::blit(view, depthCopy_, 0, 0, bgfx::getTexture(target, 1));
        ++copies_.depth;
    }
}

void Renderer::destroyCopies()
{
    for (bgfx::TextureHandle* copy : {&screenCopy_, &depthCopy_}) {
        if (bgfx::isValid(*copy))
            bgfx::destroy(*copy);
        *copy = BGFX_INVALID_HANDLE;
    }
}

void Renderer::render(const Scene& scene, const std::filesystem::path& projectRoot, bgfx::ViewId view,
                      const float* viewMtx, const float* projMtx, bgfx::ViewId shadowViews, const Scene* extra)
{
    setProjectRoot(projectRoot);
    bgfx::setViewTransform(view, viewMtx, projMtx);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential); // blended meshes must draw in the order submitted
    const entt::registry& registry = scene.registry();
    float cameraWorld[16];
    bx::mtxInverse(cameraWorld, viewMtx);
    const bx::Vec3 eye{cameraWorld[12], cameraWorld[13], cameraWorld[14]};
    const float cameraPos[4] = {eye.x, eye.y, eye.z, 1.0f};
    const bgfx::FrameBufferHandle target = std::exchange(target_, BGFX_INVALID_HANDLE); // for this render() only
    // Blended draws go to blendView: the copies blit at its start, after view's opaque meshes and sky.
    const bgfx::ViewId blendView = bgfx::isValid(target) ? static_cast<bgfx::ViewId>(view + 1) : view;
    bool screenCopied = false, depthCopied = false;

    float lightDir[4] = {0.0f, -1.0f, 0.0f, 0.0f};
    float lightColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const DirectionalLight* sun = nullptr;
    if (const entt::entity entity = firstDepthFirst<DirectionalLight>(scene); entity != entt::null) {
        const DirectionalLight* light = &registry.get<DirectionalLight>(entity);
        sun = light;
        float world[16];
        scene.worldMatrix(entity, world);
        const bx::Vec3 dir = bx::normalize(transformDir({0.0f, 0.0f, -1.0f}, world));
        lightDir[0] = dir.x;
        lightDir[1] = dir.y;
        lightDir[2] = dir.z;
        lightDir[3] = 1.0f;
        lightColor[0] = light->color.x * light->intensity;
        lightColor[1] = light->color.y * light->intensity;
        lightColor[2] = light->color.z * light->intensity;
    }
    const SceneLighting& lighting = scene.lighting;
    const float ambient[4] = {lighting.ambientColor.x * lighting.ambientIntensity,
                              lighting.ambientColor.y * lighting.ambientIntensity,
                              lighting.ambientColor.z * lighting.ambientIntensity, 0.0f};
    const ScenePostProcessing::Fog& fog = scene.postProcessing.fog;
    const float fogColor[4] = {fog.color.x, fog.color.y, fog.color.z, fog.enabled ? 1.0f : 0.0f};
    const float fogParams[4] = {fog.density, fog.start, 0.0f, 0.0f};

    // Environments: [0] outside every probe (the sky's, tinted by the ambient; none with the sky off), then one per baked
    // probe. probeEnvironment: per box, its index in environments (0 if it can't be used: unbaked, missing, corrupt).
    sky_.update(scene.sky, root_);
    updateSkyEnvironment(scene.sky, view);
    std::vector<Environment> environments(1);
    const bx::Vec3 ambientTint{ambient[0], ambient[1], ambient[2]};
    if (bgfx::isValid(skyEnv_))
        environments[0] = {skyEnv_, static_cast<float>(envMips(kSkyEnvSize) - 1), scaleSh9(skySh_, ambientTint), ambientTint};
    std::vector<ProbeBox> probeBoxes;
    std::vector<size_t> probeEnvironment;
    for (auto [entity, probe] : registry.view<ReflectionProbe>().each()) {
        if (capture_)
            break; // one bounce: a bake sees no probes
        float world[16];
        scene.worldMatrix(entity, world);
        const ProbeBox box{{world[12], world[13], world[14]}, bx::mul(probe.boxSize, 0.5f), probe.priority};
        probeBoxes.push_back(box);
        probeEnvironment.push_back(0);
        const std::string& name = registry.get<Name>(entity).value;
        if (probe.bakedPath.empty()) {
            warnOnce(std::format("Reflection probe '{}': not baked; meshes in its box are lit as outside it", name));
            continue;
        }
        const ProbeFile& file = probeFile(probe);
        if (!file.error.empty()) {
            warnOnce(std::format("Reflection probe '{}': '{}': {}; meshes in its box are lit as outside it", name,
                                 probe.bakedPath, file.error));
            continue;
        }
        const bx::Vec3 tint{probe.intensity, probe.intensity, probe.intensity};
        environments.push_back({file.cube, static_cast<float>(envMips(static_cast<uint16_t>(probe.resolution)) - 1),
                                scaleSh9(file.sh, tint), tint, true, box.center, box.halfSize});
        probeEnvironment.back() = environments.size() - 1;
    }
    // The environment of a draw: its probe's (by its bounds' center), else environments[0].
    const auto environmentOf = [&](const DrawItem& item) -> size_t {
        const std::optional<size_t> probe = selectProbe(probeBoxes, bx::mul(bx::add(item.bounds.min, item.bounds.max), 0.5f));
        return probe ? probeEnvironment[*probe] : 0;
    };

    // Point, then spot, then area lights (scale ignored: only the world matrix's position and axis directions count).
    std::vector<LocalLightSample> localLights;
    for (auto [entity, light] : registry.view<PointLight>().each()) {
        float world[16];
        scene.worldMatrix(entity, world);
        localLights.push_back({.position = {world[12], world[13], world[14]},
                               .range = light.range,
                               .color = bx::mul(light.color, light.intensity),
                               .castShadows = light.castShadows,
                               .bias = light.bias,
                               .baked = light.bakeMode == LightBakeMode::Full});
    }
    for (auto [entity, light] : registry.view<SpotLight>().each()) {
        float world[16];
        scene.worldMatrix(entity, world);
        localLights.push_back({.position = {world[12], world[13], world[14]},
                               .range = light.range,
                               .color = bx::mul(light.color, light.intensity),
                               .castShadows = light.castShadows,
                               .bias = light.bias,
                               .type = LocalLightType::Spot,
                               .direction = bx::normalize(transformDir({0.0f, 0.0f, -1.0f}, world)),
                               .cosInner = std::cos(bx::toRad(light.innerAngle)),
                               .cosOuter = std::cos(bx::toRad(light.outerAngle)),
                               .baked = light.bakeMode == LightBakeMode::Full});
    }
    for (auto [entity, light] : registry.view<AreaLight>().each()) {
        float world[16];
        scene.worldMatrix(entity, world);
        localLights.push_back(
            {.position = {world[12], world[13], world[14]},
             .range = light.range,
             .color = bx::mul(light.color, light.intensity),
             .castShadows = light.castShadows,
             .bias = light.bias,
             .type = LocalLightType::Area,
             .direction = bx::normalize(transformDir({0.0f, 0.0f, -1.0f}, world)),
             .halfWidth = bx::mul(bx::normalize(transformDir({1.0f, 0.0f, 0.0f}, world)), 0.5f * light.width),
             .halfHeight = bx::mul(bx::normalize(transformDir({0.0f, 1.0f, 0.0f}, world)), 0.5f * light.height),
             .twoSided = light.twoSided,
             .baked = light.bakeMode == LightBakeMode::Full});
    }
    // The local lights a lightmapped draw still gets (lightShinesOn: not the Full ones), by index into localLights.
    std::vector<size_t> realtimeLights;
    std::vector<LocalLightSample> realtimeSamples;
    for (size_t i = 0; i < localLights.size(); ++i)
        if (lightShinesOn(localLights[i].baked ? LightBakeMode::Full : LightBakeMode::Indirect, true)) {
            realtimeLights.push_back(i);
            realtimeSamples.push_back(localLights[i]);
        }
    const bool sunBaked = sun && sun->bakeMode == LightBakeMode::Full;

    // GI (docs/engine/lightmaps.html): the scene's bake, if on and it loads (the extra scene's meshes get none: its ids
    // aren't the bake's).
    const SceneLightmaps& gi = scene.lighting.lightmaps;
    LightmapSet* bake = gi.enabled && !gi.bakedPath.empty() ? &lightmapSet(gi.bakedPath) : nullptr;
    if (bake && !bake->error.empty())
        bake = nullptr;
    constexpr const char* kPrimitiveNames[] = {"primitive:cube", "primitive:plane", "primitive:sphere"}; // by Primitive

    // SDFGI (sdfgi.h): on, it gathers the scene's Static shapes as its draws are made.
    const SceneSdfgi& sdfgiSettings = scene.lighting.sdfgi;
    const bool sdfgiOn = sdfgiSettings.enabled && !capture_ && sdfgi_->supported();
    if (sdfgiSettings.enabled && !capture_ && !sdfgi_->supported())
        warnOnce("SDFGI: this GPU lacks compute or the 3D image formats it needs; SDFGI is off");
    if (!sdfgiOn && sdfgi_->ready())
        sdfgi_->reset();
    std::vector<SdfgiShape> sdfgiShapes;
    const auto addShape = [&](const DrawItem& item, SdfgiShapeType type, const bx::Vec3& half) {
        const bx::Vec3 albedo{item.material->baseColor[0], item.material->baseColor[1], item.material->baseColor[2]};
        sdfgiShapes.push_back(sdfgiShape(type, item.world.data(), half, albedo, emissiveRadiance(*item.material)));
    };

    std::vector<DrawItem> draws;
    std::vector<std::vector<Mat4>> boneSets;
    const auto addDraws = [&](const Scene& from, bool baked) {
        const entt::registry& entities = from.registry();
        WorldMatrices worldOf(from);
        for (auto [entity, mesh] : entities.view<MeshRenderer>().each()) {
            const auto* instance = entities.try_get<MaterialInstance>(entity);
            const Mesh& gpu = meshes_[static_cast<size_t>(mesh.primitive)];
            DrawItem item;
            item.world = worldOf(entity);
            item.mirrored = isMirrored(item.world);
            item.bounds = transformBounds(primitiveBounds(mesh.primitive), item.world.data());
            item.material = instance ? &instance->material : &materials_.get(mesh.material);
            item.vertices = gpu.vertices;
            item.indices = gpu.indices;
            item.castShadows = mesh.castShadows;
            item.receiveShadows = mesh.receiveShadows;
            if (baked && mesh.giMode == GiMode::Static) // unbaked: the probe grid, if any
                item.giDynamic = !(bake && applyLightmap(*bake, entities.get<PersistentId>(entity).value,
                                                         kPrimitiveNames[static_cast<size_t>(mesh.primitive)], 0, item));
            item.giDynamic = item.giDynamic || (baked && mesh.giMode == GiMode::Dynamic);
            if (sdfgiOn && &from == &scene && mesh.giMode == GiMode::Static)
                addShape(item, mesh.primitive == Primitive::Sphere ? SdfgiShapeType::Ellipsoid : SdfgiShapeType::Box,
                         mesh.primitive == Primitive::Plane ? bx::Vec3{0.5f, 0.0f, 0.5f} : bx::Vec3{0.5f, 0.5f, 0.5f});
            draws.push_back(item);
        }
        for (auto [entity, model] : entities.view<ModelRenderer>().each()) {
            const size_t first = draws.size();
            addModelDraws(from, entity, model, worldOf(entity).data(), draws, boneSets);
            if (sdfgiOn && &from == &scene && model.giMode == GiMode::Static)
                for (size_t i = first; i < draws.size(); ++i)
                    if (draws[i].bones < 0) {
                        const Material& material = *draws[i].material;
                        sdfgiShapes.push_back(sdfgiBoxShape(
                            draws[i].bounds, {material.baseColor[0], material.baseColor[1], material.baseColor[2]},
                            emissiveRadiance(material)));
                    }
            if (!baked || model.giMode == GiMode::Off)
                continue;
            const EntityId id = entities.get<PersistentId>(entity).value;
            for (size_t i = first; i < draws.size(); ++i) {
                DrawItem& item = draws[i];
                if (model.giMode == GiMode::Static && item.bones >= 0)
                    warnOnce(std::format("'{}': a skinned model can't be Static; lit as Dynamic",
                                         entities.get<Name>(entity).value));
                item.giDynamic = !(model.giMode == GiMode::Static && item.bones < 0 && bake &&
                                   applyLightmap(*bake, id, "model:" + model.model, i - first, item));
            }
        }
        addTerrainDraws(from, draws);
    };
    addDraws(scene, gi.enabled);
    if (extra)
        addDraws(*extra, false);
    pruneTerrain();
    resolveShaders(draws);
    for (DrawItem& item : draws) {
        // Lightmaps off (terrain tiles carry theirs), or a .kshader using the lightmap's stages: the probe grid instead.
        const bool usable = gi.enabled && (!item.shader || shaderTakesLightmap(item.shader->source->source));
        if (bgfx::isValid(item.lightmap) && !usable) {
            item.lightmap = item.lightmapDirection = BGFX_INVALID_HANDLE;
            item.giDynamic = gi.enabled;
        }
    }
    // The local lights of a draw: the nearest that reach it, but for the Full ones on a lightmapped draw.
    const auto lightsOf = [&](const DrawItem& item) {
        if (!bgfx::isValid(item.lightmap))
            return nearestLocalLights(localLights, item.bounds);
        std::vector<size_t> nearest = nearestLocalLights(realtimeSamples, item.bounds);
        for (size_t& i : nearest)
            i = realtimeLights[i];
        return nearest;
    };

    const ShadowFrame shadows = renderShadows(sun, {lightDir[0], lightDir[1], lightDir[2]}, draws, boneSets,
                                              localLights, viewMtx, projMtx, shadowViews);
    // Local light index to its shadow: a point light's cube 0..3, a spot / area light's u_csmMtx tile, -1 none.
    std::vector<float> shadowOf(localLights.size(), -1.0f);
    for (size_t cube = 0; cube < shadows.pointLights.size(); ++cube)
        shadowOf[shadows.pointLights[cube]] = static_cast<float>(cube);
    for (size_t tile = 0; tile < shadows.tileLights.size(); ++tile)
        shadowOf[shadows.tileLights[tile]] = static_cast<float>(kMaxShadowCascades + tile);
    const bool atlas = shadows.sun || !shadows.tileLights.empty();

    // VoxelGI (voxel_gi.h): each enabled, baked volume relit for this frame by every light, occluded by the shadow maps
    // just rendered, in the view after them. volumeBoxes: their grids' boxes, as draws select them (selectProbe).
    struct VoxelVolume {
        VoxelGrid grid;
        bgfx::TextureHandle radiance = BGFX_INVALID_HANDLE;
        uint8_t mips = 1;
        float intensity = 1.0f;
        VoxelGiPreset preset;
        std::array<std::array<float, 4>, kMaxVoxelGiCones> cones{}; // u_voxelCones: voxelGiCones(preset.cones)
    };
    std::vector<VoxelVolume> volumes;
    std::vector<ProbeBox> volumeBoxes;
    const auto giView = static_cast<bgfx::ViewId>(shadowViews + kShadowViews - 2);
    for (auto [entity, component] : registry.view<VoxelGI>().each()) {
        if (capture_ || !component.enabled)
            continue; // one bounce: a probe bake sees no volume
        const std::string& name = registry.get<Name>(entity).value;
        if (component.bakedPath.empty()) {
            warnOnce(std::format("VoxelGI '{}': not baked; meshes in its box are lit as outside it", name));
            continue;
        }
        if (!voxelGi_->passes.supported())
            continue; // warned once at init
        const VoxelGiState::File& file = voxelGi_->file(root_, component);
        if (!file.error.empty()) {
            warnOnce(std::format("VoxelGI '{}': '{}': {}; meshes in its box are lit as outside it", name,
                                 component.bakedPath, file.error));
            continue;
        }
        const VoxelGrid& grid = file.volume.grid;
        const bx::Vec3 sunDir{lightDir[0], lightDir[1], lightDir[2]};
        const VoxelGiLights lights =
            packVoxelGiLights(sun ? &sunDir : nullptr, {lightColor[0], lightColor[1], lightColor[2]}, shadows.sun,
                              localLights, shadowOf, {grid.origin, bx::add(grid.origin, grid.extent())});
        VoxelGiRenderer::Shadows maps{.atlas = atlas ? csm_ : bgfx::TextureHandle BGFX_INVALID_HANDLE,
                                      .csmMtx = shadows.csmMtx[0],
                                      .csmBias = shadows.csmBias[0],
                                      .cascades = shadows.sun ? std::clamp(sun->cascadeCount, 1, kMaxShadowCascades) : 0};
        for (size_t cube = 0; cube < shadows.pointLights.size(); ++cube)
            maps.cubes[cube] = cubes_[cube];
        const VoxelGiPreset preset = voxelGiPreset(component.quality);
        volumes.push_back({grid, voxelGi_->passes.update(giView, file.volume, preset, lights, maps), file.volume.mips,
                           component.intensity, preset});
        const std::vector<std::array<float, 4>> cones = voxelGiCones(preset.cones);
        std::ranges::copy(cones, volumes.back().cones.begin());
        volumeBoxes.push_back(voxelGiBox(grid, component.priority));
    }
    if (!volumes.empty()) {
        bgfx::setViewName(giView, "VoxelGI");
        bgfx::setViewMode(giView, bgfx::ViewMode::Sequential);
    }
    // SDFGI (sdfgi.h): its cascades around the eye scrolled, rebuilt where Static shapes or terrain tiles changed,
    // relit and their probes updated, in the last shadow view.
    if (sdfgiOn) {
        SdfgiRenderer::Frame frame{.eye = eye, .settings = sdfgiSettings, .shapes = std::move(sdfgiShapes)};
        std::map<std::pair<int, int>, const TerrainTileData*> tiles;
        std::vector<const TerrainTile*> streamed; // the scene's tiles, and the extra scene's (the editor's open zone)
        for (const Scene* from : {&scene, extra})
            if (from)
                for (auto [entity, tile] : from->registry().view<TerrainTile>().each())
                    streamed.push_back(&tile);
        for (const TerrainTile* streamedTile : streamed) {
            const TerrainTile& tile = *streamedTile;
            if (!tile.data)
                continue;
            const float x0 = float(tile.tx) * map::kTileSize, z0 = float(tile.tz) * map::kTileSize;
            frame.tiles.push_back({tile.map, tile.tx, tile.tz, tile.data.get(), x0, z0, x0 + map::kTileSize,
                                   z0 + map::kTileSize});
            tiles[{tile.tx, tile.tz}] = tile.data.get();
            const std::string& material = tile.data->map->material;
            const std::array<float, 4>& color = materials_.get(material).baseColor;
            frame.terrainAlbedo = {color[0], color[1], color[2]};
        }
        // ponytail: tiles by (tx, tz) alone: one map streamed at a time, as TerrainStreamer does.
        if (!tiles.empty())
            frame.heightAt = [tiles](float x, float z) -> std::optional<float> {
                const int tx = int(std::floor(x / map::kTileSize)), tz = int(std::floor(z / map::kTileSize));
                const auto it = tiles.find({tx, tz});
                if (it == tiles.end())
                    return std::nullopt;
                return map::ground(it->second->heights, *it->second->map, x - float(tx) * map::kTileSize,
                                   z - float(tz) * map::kTileSize);
            };
        const bx::Vec3 sunDir{lightDir[0], lightDir[1], lightDir[2]};
        frame.sunDirection = sun ? &sunDir : nullptr;
        frame.sunColor = {lightColor[0], lightColor[1], lightColor[2]};
        frame.lights = localLights;
        frame.sky = bgfx::isValid(environments[0].cube);
        frame.skySh = environments[0].sh;
        frame.ambient = {ambient[0], ambient[1], ambient[2]};
        sdfgi_->update(static_cast<bgfx::ViewId>(shadowViews + kShadowViews - 1), frame);
    }
    const bool sdfgiReady = sdfgiOn && sdfgi_->ready();

    // The volume a draw cone-traces: the one its bounds' center selects, if its shader leaves VoxelGI its stage.
    const auto volumeOf = [&](const DrawItem& item) -> int {
        if (volumeBoxes.empty() || (item.shader && !shaderTakesVoxelGi(item.shader->source->source)))
            return -1;
        const std::optional<size_t> volume =
            selectProbe(volumeBoxes, bx::mul(bx::add(item.bounds.min, item.bounds.max), 0.5f));
        return volume ? static_cast<int>(*volume) : -1;
    };
    // A draw's diffuse GI (the selection point, lightmap.h selectDiffuseGi): its lightmap, else the VoxelGI volume its
    // bounds' center is in, else SDFGI if on (built-in shader only: its SDFGI variant), else the probe grid there if it
    // samples it, else the environment. A volume's specular too, and SDFGI's rough reflections outside reflection probes.
    const auto giOf = [&](const DrawItem& item) {
        std::optional<Sh9> grid;
        if (item.giDynamic && bake)
            grid = sampleProbeGrid(bake->sidecar.grid, bx::mul(bx::add(item.bounds.min, item.bounds.max), 0.5f));
        const int volume = volumeOf(item);
        const bool sdfgi = sdfgiReady && !item.shader; // the built-in shader's SDFGI variant
        GiDraw draw{selectDiffuseGi(bgfx::isValid(item.lightmap), grid.has_value(), volume >= 0, sdfgi)};
        draw.sdfgiSpecular = draw.source == DiffuseGi::Sdfgi && environmentOf(item) == 0;
        draw.volume = volume;
        draw.grid = grid.has_value();
        if (grid)
            draw.sh = *grid;
        return draw;
    };

    // Everything a mesh draw sets but its transform: material, lights (nearest: lightsOf), environment (an index in
    // environments), GI (giOf), shadows, buffers.
    const auto setDrawState = [&](const DrawItem& item, const std::vector<size_t>& nearest, size_t environment,
                                  const GiDraw& giDraw) {
        const Material& material = *item.material;
        // Unused slots: black, range 1, so they add nothing. Packed as fs_mesh.sc's u_local* comments say.
        float localPosRange[kMaxLocalLights][4] = {};
        float localColor[kMaxLocalLights][4] = {};
        float localAxis[kMaxLocalLights][4] = {};
        float localShadow[kMaxLocalLights][4] = {};
        for (auto& slot : localPosRange)
            slot[3] = 1.0f;
        for (auto& slot : localShadow)
            slot[0] = -1.0f;
        for (size_t slot = 0; slot < nearest.size(); ++slot) {
            const LocalLightSample& light = localLights[nearest[slot]];
            std::memcpy(localPosRange[slot], &light.position, sizeof(bx::Vec3));
            localPosRange[slot][3] = light.range;
            std::memcpy(localColor[slot], &light.color, sizeof(bx::Vec3));
            localShadow[slot][0] = shadowOf[nearest[slot]];
            localShadow[slot][1] = light.bias;
            switch (light.type) {
            case LocalLightType::Point:
                localShadow[slot][2] = light.range;
                break;
            case LocalLightType::Spot:
                localColor[slot][3] = 1.0f;
                std::memcpy(localAxis[slot], &light.direction, sizeof(bx::Vec3));
                localShadow[slot][2] = light.cosInner;
                localShadow[slot][3] = light.cosOuter;
                break;
            case LocalLightType::Area:
                localColor[slot][3] = light.twoSided ? 3.0f : 2.0f;
                std::memcpy(localAxis[slot], &light.halfWidth, sizeof(bx::Vec3));
                localAxis[slot][3] = light.halfHeight.z;
                localShadow[slot][2] = light.halfHeight.x;
                localShadow[slot][3] = light.halfHeight.y;
                break;
            }
        }
        const float shadowParams[4] = {item.receiveShadows ? 1.0f : 0.0f, shadows.sun ? 1.0f : 0.0f, 0.0f, 0.0f};
        // Packed as fs_mesh.sc's u_env* comments say; no cube: u_envSpecular.w 0, the flat ambient.
        const Environment& env = environments[environment];
        const bool lit = bgfx::isValid(env.cube);
        float envSh[9][4] = {};
        for (size_t i = 0; i < 9; ++i)
            std::memcpy(envSh[i], &env.sh[i * 3], 3 * sizeof(float));
        const float envSpecular[4] = {env.specular.x, env.specular.y, env.specular.z, lit ? env.maxLod : 0.0f};
        const float envBox[2][4] = {{env.center.x, env.center.y, env.center.z, env.box ? 1.0f : 0.0f},
                                    {env.halfSize.x, env.halfSize.y, env.halfSize.z, 0.0f}};
        const bool blend = material.alphaMode == AlphaMode::Blend;
        const bool additive = material.alphaMode == AlphaMode::Additive;

        // Packed as fs_mesh.sc's u_gi says. A Full sun's light is in a lightmapped draw's lightmap: none for it.
        const bool lightmapped = giDraw.source == DiffuseGi::Lightmap;
        const float giParams[4] = {float(giDraw.source), gi.intensity, gi.directional ? 1.0f : 0.0f, 0.0f};
        const bool sunOff = lightmapped && sunBaked;
        const float noSun[4] = {lightDir[0], lightDir[1], lightDir[2], 0.0f};
        const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};

        setMaterialState(item);
        bgfx::setUniform(cameraPos_, cameraPos);
        bgfx::setUniform(fogColor_, fogColor);
        bgfx::setUniform(fogParams_, fogParams);
        bgfx::setUniform(lightDir_, sunOff ? noSun : lightDir);
        bgfx::setUniform(lightColor_, sunOff ? black : lightColor);
        bgfx::setUniform(gi_, giParams);
        if (giDraw.source == DiffuseGi::ProbeGrid ||
            ((giDraw.source == DiffuseGi::VoxelGi || giDraw.source == DiffuseGi::Sdfgi) && giDraw.grid)) {
            float giSh[9][4] = {};
            for (size_t i = 0; i < 9; ++i)
                std::memcpy(giSh[i], &giDraw.sh[i * 3], 3 * sizeof(float));
            bgfx::setUniform(giSh_, giSh, 9);
        }
        // Packed as fs_mesh.sc's u_voxelGi says; every draw sets it (zeros: no volume), as uniforms outlive their draw.
        float voxelGi[4][4] = {};
        if (giDraw.volume >= 0) {
            const VoxelVolume& volume = volumes[static_cast<size_t>(giDraw.volume)];
            const bx::Vec3 extent = volume.grid.extent();
            const VoxelGiPreset& p = volume.preset;
            const float packed[4][4] = {
                {volume.grid.origin.x, volume.grid.origin.y, volume.grid.origin.z, volume.grid.voxelSize},
                {1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z, volume.intensity},
                {float(volume.mips - 1), float(p.cones), p.stepScale, float(p.specularSteps)},
                {p.aperture, giDraw.grid ? 1.0f : 0.0f, float(p.diffuseSteps), 1.0f}};
            std::memcpy(voxelGi, packed, sizeof(voxelGi));
            bgfx::setUniform(voxelGi_->cones, volume.cones.data(), kMaxVoxelGiCones);
            bgfx::setTexture(11, voxelGi_->sampler, volume.radiance);
        }
        bgfx::setUniform(voxelGi_->params, voxelGi, 4);
        sdfgi_->bind(giDraw.source == DiffuseGi::Sdfgi, giDraw.grid, giDraw.sdfgiSpecular); // zeros unless SDFGI
        if (lightmapped) {
            bgfx::setUniform(lightmapST_, item.lightmapST.data());
            bgfx::setTexture(12, lightmapSampler_, item.lightmap);
            bgfx::setTexture(13, lightmapDirSampler_, item.lightmapDirection);
        }
        bgfx::setUniform(ambient_, ambient);
        bgfx::setUniform(envSh_, envSh, 9);
        bgfx::setUniform(envSpecular_, envSpecular);
        bgfx::setUniform(envBox_, envBox, 2);
        bgfx::setTexture(8, envSampler_, lit ? env.cube : blackCube_);
        bgfx::setUniform(localPosRange_, localPosRange, kMaxLocalLights);
        bgfx::setUniform(localColor_, localColor, kMaxLocalLights);
        bgfx::setUniform(localAxis_, localAxis, kMaxLocalLights);
        bgfx::setUniform(localShadow_, localShadow, kMaxLocalLights);
        bgfx::setUniform(shadowParams_, shadowParams);
        if (atlas)
            bgfx::setUniform(csmMtx_, shadows.csmMtx, kMaxShadowCascades + kMaxShadowedLocalLights);
        if (shadows.sun) {
            bgfx::setUniform(csmSplits_, shadows.csmSplits);
            bgfx::setUniform(csmBias_, shadows.csmBias, kMaxShadowCascades);
        }
        if (item.shader && item.shader->source->source.screenTexture)
            bgfx::setTexture(kScreenTextureStage, screenSampler_, screenCopied ? screenCopy_ : black_);
        if (item.shader && item.shader->source->source.depthTexture)
            bgfx::setTexture(kDepthTextureStage, depthSampler_, depthCopied ? depthCopy_ : white_);
        bgfx::setTexture(2, csmSampler_, atlas ? csm_ : noShadow2D_);
        for (size_t cube = 0; cube < kMaxShadowedPointLights; ++cube)
            bgfx::setTexture(static_cast<uint8_t>(3 + cube), pointShadowSamplers_[cube],
                             cube < shadows.pointLights.size() ? cubes_[cube] : noShadowCube_);
        bgfx::setVertexBuffer(0, item.vertices);
        bgfx::setIndexBuffer(item.indices);
        bgfx::setState((additive ? kAdditiveState : blend ? kBlendState : kState) | cullState(material.cullMode, item.mirrored));
    };
    const auto submit = [&](bgfx::ViewId to, const DrawItem& item, const std::vector<size_t>& nearest) {
        float normal[16];
        normalMatrix(item.world, normal);
        const GiDraw giDraw = giOf(item);
        setDrawState(item, nearest, environmentOf(item), giDraw);
        bgfx::setTransform(item.world.data());
        bgfx::setUniform(normalMatrix_, normal);
        if (item.bones >= 0) {
            const std::vector<Mat4>& bones = boneSets[static_cast<size_t>(item.bones)];
            bgfx::setUniform(bones_, bones.data(), static_cast<uint16_t>(bones.size()));
        }
        if (item.shader)
            bgfx::submit(to, item.bones >= 0 ? item.shader->skinned : item.shader->mesh);
        else
            bgfx::submit(to, giDraw.source == DiffuseGi::Sdfgi ? sdfgiPrograms_[item.bones >= 0 ? 1 : 0]
                             : item.bones >= 0                   ? skinnedProgram_
                                                                 : meshProgram_);
    };

    // Opaque and mask meshes in the view now, batched; blended ones after, one by one far to near by world-space
    // bounds center.
    float viewProj[16];
    bx::mtxMul(viewProj, viewMtx, projMtx);
    const Frustum frustum = frustumFromViewProj(viewProj, bgfx::getCaps()->homogeneousDepth);
    std::vector<const DrawItem*> opaque;
    std::vector<std::vector<size_t>> opaqueLights; // per opaque draw
    std::vector<size_t> opaqueEnvironments;        // the same
    std::vector<GiDraw> opaqueGi;                  // the same
    std::vector<size_t> opaqueGiKeys;              // BatchKey::gi
    std::vector<const DrawItem*> blended;
    std::vector<bx::Vec3> centers;
    for (const DrawItem& item : draws) {
        if (culled(item, frustum))
            continue;
        if (!isTransparent(*item.material)) {
            opaque.push_back(&item);
            opaqueLights.push_back(lightsOf(item));
            opaqueEnvironments.push_back(environmentOf(item));
            opaqueGi.push_back(giOf(item));
            // A lightmap batches with its texture; each probe-grid draw has its own harmonics, so batches alone (as does
            // a VoxelGI or SDFGI draw fading to them); a VoxelGI volume batches with itself, SDFGI draws (by reflection
            // or not) with each other.
            const GiDraw& drawGi = opaqueGi.back();
            const bool own = drawGi.source == DiffuseGi::ProbeGrid ||
                             ((drawGi.source == DiffuseGi::VoxelGi || drawGi.source == DiffuseGi::Sdfgi) && drawGi.grid);
            opaqueGiKeys.push_back((drawGi.source == DiffuseGi::Lightmap ? size_t(item.lightmap.idx) + 1
                                    : own                            ? (size_t(1) << 20) + opaque.size()
                                    : drawGi.source == DiffuseGi::Sdfgi ? (size_t(1) << 30) + (drawGi.sdfgiSpecular ? 1 : 0)
                                                                     : 0) +
                                   (drawGi.volume >= 0 ? size_t(drawGi.volume + 1) << 40 : 0));
            continue;
        }
        blended.push_back(&item);
        centers.push_back(bx::mul(bx::add(item.bounds.min, item.bounds.max), 0.5f));
    }
    // Each emitter is one blended draw at its position, sorted with the blended meshes (its particles aren't sorted).
    const std::vector<ParticleRenderer::Draw> particles =
        capture_ ? std::vector<ParticleRenderer::Draw>() : particles_.simulate(scene, view, culling_ ? &frustum : nullptr);
    for (const ParticleRenderer::Draw& emitter : particles)
        centers.push_back(emitter.center);
    std::vector<const DrawItem*> batch;
    for (const std::vector<size_t>& indices :
         batchDraws(opaque.size(), [&](size_t i) {
             return batchKey(*opaque[i], opaqueLights[i], opaqueEnvironments[i], opaqueGiKeys[i]);
         })) {
        batch.clear();
        for (size_t i : indices)
            batch.push_back(opaque[i]);
        bgfx::InstanceDataBuffer instances;
        if (instanced(batch, instances)) {
            setDrawState(*batch.front(), opaqueLights[indices.front()], opaqueEnvironments[indices.front()],
                         opaqueGi[indices.front()]);
            bgfx::setInstanceDataBuffer(&instances);
            bgfx::submit(view, batch.front()->shader                                    ? batch.front()->shader->instanced
                               : opaqueGi[indices.front()].source == DiffuseGi::Sdfgi ? sdfgiPrograms_[2]
                                                                                       : instancedProgram_);
            continue;
        }
        for (size_t i : indices)
            submit(view, *opaque[i], opaqueLights[i]);
    }
    sky_.draw(scene.sky, view, viewMtx, projMtx); // behind the opaque meshes, under the blended ones
    if (blendView != view) {
        bgfx::setViewName(blendView, "Scene blended");
        bgfx::setViewFrameBuffer(blendView, target);
        bgfx::setViewRect(blendView, 0, 0, targetWidth_, targetHeight_);
        bgfx::setViewClear(blendView, BGFX_CLEAR_NONE);
        bgfx::setViewTransform(blendView, viewMtx, projMtx);
        bgfx::setViewMode(blendView, bgfx::ViewMode::Sequential);
        // Copied only for a blended .kshader in view that samples them (opaque and mask ones may not: resolveShaders).
        for (const DrawItem* item : blended) {
            screenCopied = screenCopied || (item->shader && item->shader->source->source.screenTexture);
            depthCopied = depthCopied || (item->shader && item->shader->source->source.depthTexture);
        }
        copyScene(blendView, target, screenCopied, depthCopied);
    }
    for (size_t i : backToFront(centers, eye)) {
        if (i < blended.size()) {
            submit(blendView, *blended[i], lightsOf(*blended[i]));
            continue;
        }
        const ParticleRenderer::Draw& emitter = particles[i - blended.size()];
        const bgfx::TextureHandle image = texture(emitter.emitter->texture, TextureUsage::Color); // white_: none or failed to load
        particles_.draw(emitter, blendView, image, image.idx != white_.idx, fogColor, fogParams, cameraPos);
    }
}

void Renderer::renderOverlays(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx,
                              entt::entity selected)
{
    bgfx::setViewTransform(view, viewMtx, projMtx);
    // ponytail: blended meshes don't write depth, so overlays draw over glass; draw them before blended meshes in
    // a separate HDR pass if that matters.
    drawGrid(view);
    drawCameraFrustums(scene, view);
    drawLightGizmo(scene, view, selected);
    drawProbeBox(scene, view, selected);
    drawColliderWireframe(scene, view, selected);
    drawParticleShape(scene, view, selected);
}

void Renderer::drawGrid(bgfx::ViewId view)
{
    bgfx::setVertexBuffer(0, grid_);
    bgfx::setState(kState | BGFX_STATE_PT_LINES);
    bgfx::submit(view, lineProgram_);
}

void Renderer::drawCameraFrustums(const Scene& scene, bgfx::ViewId view)
{
    std::vector<LineVertex> lines;
    for (auto [entity, camera] : scene.registry().view<Camera>().each()) {
        // The inverse view matrix, so the preview matches what the camera renders (scale ignored).
        float cameraView[16], cameraProj[16], world[16];
        cameraMatrices(scene, entity, kFrustumPreviewAspect, false, cameraView, cameraProj);
        bx::mtxInverse(world, cameraView);
        const float depth = bx::min(kFrustumPreviewDistance, camera.farPlane);
        const float halfHeight = depth * std::tan(bx::toRad(camera.verticalFovDegrees) * 0.5f);
        const float halfWidth = halfHeight * kFrustumPreviewAspect;
        const bx::Vec3 origin = bx::mul(bx::Vec3{0.0f, 0.0f, 0.0f}, world);
        const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        bx::Vec3 corners[4] = {origin, origin, origin, origin};
        for (int i = 0; i < 4; ++i)
            corners[i] = bx::mul(bx::Vec3{signs[i][0] * halfWidth, signs[i][1] * halfHeight, -depth}, world);
        for (int i = 0; i < 4; ++i) {
            const bx::Vec3& a = corners[i];
            const bx::Vec3& b = corners[(i + 1) % 4];
            lines.push_back({origin.x, origin.y, origin.z, kFrustumColor});
            lines.push_back({a.x, a.y, a.z, kFrustumColor});
            lines.push_back({a.x, a.y, a.z, kFrustumColor});
            lines.push_back({b.x, b.y, b.z, kFrustumColor});
        }
    }
    submitLines(lines, lineLayout_, lineProgram_, view);
}

// The selected light's gizmo, at its world position along its world axes (scale ignored): a PointLight's range as
// three axis-aligned circles; a SpotLight's outer cone out to its range (four edge lines and the end ring) and the
// ring of its inner angle there; an AreaLight's rect, an arrow the way it shines and a circle of its range around it.
void Renderer::drawLightGizmo(const Scene& scene, bgfx::ViewId view, entt::entity selected)
{
    if (!scene.valid(selected))
        return;
    const entt::registry& registry = scene.registry();
    const auto* point = registry.try_get<PointLight>(selected);
    const auto* spot = registry.try_get<SpotLight>(selected);
    const auto* area = registry.try_get<AreaLight>(selected);
    if (!point && !spot && !area)
        return;
    float world[16];
    scene.worldMatrix(selected, world);
    const bx::Vec3 center{world[12], world[13], world[14]};
    const bx::Vec3 x = bx::normalize(transformDir({1.0f, 0.0f, 0.0f}, world));
    const bx::Vec3 y = bx::normalize(transformDir({0.0f, 1.0f, 0.0f}, world));
    const bx::Vec3 forward = bx::normalize(transformDir({0.0f, 0.0f, -1.0f}, world)); // the way spot / area lights shine
    std::vector<LineVertex> lines;
    const auto line = [&](const bx::Vec3& a, const bx::Vec3& b) {
        lines.push_back({a.x, a.y, a.z, kLightGizmoColor});
        lines.push_back({b.x, b.y, b.z, kLightGizmoColor});
    };
    // Around c in the plane of unit axes a and b.
    const auto circle = [&](const bx::Vec3& c, const bx::Vec3& a, const bx::Vec3& b, float radius) {
        const auto at = [&](int segment) {
            const float angle = 2.0f * bx::kPi * static_cast<float>(segment) / kRangeCircleSegments;
            return bx::add(c, bx::add(bx::mul(a, std::cos(angle) * radius), bx::mul(b, std::sin(angle) * radius)));
        };
        for (int segment = 0; segment < kRangeCircleSegments; ++segment)
            line(at(segment), at(segment + 1));
    };
    if (point) {
        const bx::Vec3 axes[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        for (int axis = 0; axis < 3; ++axis)
            circle(center, axes[(axis + 1) % 3], axes[(axis + 2) % 3], point->range);
    }
    if (spot) {
        for (const float angle : {spot->innerAngle, spot->outerAngle})
            circle(bx::add(center, bx::mul(forward, spot->range * std::cos(bx::toRad(angle)))), x, y,
                   spot->range * std::sin(bx::toRad(angle)));
        const bx::Vec3 end = bx::add(center, bx::mul(forward, spot->range * std::cos(bx::toRad(spot->outerAngle))));
        const float radius = spot->range * std::sin(bx::toRad(spot->outerAngle));
        for (const bx::Vec3& side : {x, y, bx::mul(x, -1.0f), bx::mul(y, -1.0f)})
            line(center, bx::add(end, bx::mul(side, radius)));
    }
    if (area) {
        const bx::Vec3 halfWidth = bx::mul(x, 0.5f * area->width), halfHeight = bx::mul(y, 0.5f * area->height);
        const bx::Vec3 corners[4] = {
            bx::sub(bx::sub(center, halfWidth), halfHeight), bx::sub(bx::add(center, halfWidth), halfHeight),
            bx::add(bx::add(center, halfWidth), halfHeight), bx::add(bx::sub(center, halfWidth), halfHeight)};
        for (int i = 0; i < 4; ++i)
            line(corners[i], corners[(i + 1) % 4]);
        const float arrow = 0.5f * std::max(area->width, area->height) + 0.25f;
        const bx::Vec3 tip = bx::add(center, bx::mul(forward, arrow));
        const bx::Vec3 back = bx::sub(tip, bx::mul(forward, 0.25f * arrow));
        line(center, tip);
        line(tip, bx::add(back, bx::mul(x, 0.15f * arrow)));
        line(tip, bx::sub(back, bx::mul(x, 0.15f * arrow)));
        circle(center, x, y, area->range);
    }
    submitLines(lines, lineLayout_, lineProgram_, view);
}

// The selected ReflectionProbe's box: axis-aligned around its world position (rotation and scale ignored, as for its
// lighting).
void Renderer::drawProbeBox(const Scene& scene, bgfx::ViewId view, entt::entity selected)
{
    if (!scene.valid(selected))
        return;
    const auto* probe = scene.registry().try_get<ReflectionProbe>(selected);
    const auto* volume = scene.registry().try_get<VoxelGI>(selected);
    if (!probe && !volume)
        return;
    float world[16];
    scene.worldMatrix(selected, world);
    bx::Vec3 center{world[12], world[13], world[14]};
    bx::Vec3 half = probe ? bx::mul(probe->boxSize, 0.5f) : bx::Vec3{0.0f, 0.0f, 0.0f};
    if (volume) { // its voxel grid's box (whole voxels, so a little past boxSize on the shorter axes)
        const VoxelGrid grid = voxelGridFor(center, volume->boxSize, volume->resolution);
        center = grid.center();
        half = bx::mul(grid.extent(), 0.5f);
    }
    const uint32_t color = probe ? kProbeBoxColor : kVoxelGiBoxColor;
    const auto corner = [&](int i) {
        return bx::add(center, bx::Vec3{i & 1 ? half.x : -half.x, i & 2 ? half.y : -half.y, i & 4 ? half.z : -half.z});
    };
    std::vector<LineVertex> lines;
    for (int i = 0; i < 8; ++i)
        for (int axis : {1, 2, 4})
            if (!(i & axis)) { // each edge once: from the corner without that axis bit to the one with it
                const bx::Vec3 a = corner(i), b = corner(i | axis);
                lines.push_back({a.x, a.y, a.z, color});
                lines.push_back({b.x, b.y, b.z, color});
            }
    submitLines(lines, lineLayout_, lineProgram_, view);
}

// The selected entity's Collider or CharacterController shape, as physics builds it (see colliderWireframe).
void Renderer::drawColliderWireframe(const Scene& scene, bgfx::ViewId view, entt::entity selected)
{
    if (!scene.valid(selected))
        return;
    std::vector<LineVertex> lines;
    for (const auto& [a, b] : colliderWireframe(scene, selected)) {
        lines.push_back({a.x, a.y, a.z, kColliderColor});
        lines.push_back({b.x, b.y, b.z, kColliderColor});
    }
    submitLines(lines, lineLayout_, lineProgram_, view);
}

// The selected entity's ParticleEmitter shape (particleShapeLines) in its transform, scale included.
void Renderer::drawParticleShape(const Scene& scene, bgfx::ViewId view, entt::entity selected)
{
    const auto* emitter = scene.valid(selected) ? scene.registry().try_get<ParticleEmitter>(selected) : nullptr;
    if (!emitter)
        return;
    float world[16];
    scene.worldMatrix(selected, world);
    std::vector<LineVertex> lines;
    for (const auto& [a, b] : particleShapeLines(*emitter)) {
        const bx::Vec3 wa = bx::mul(a, world), wb = bx::mul(b, world);
        lines.push_back({wa.x, wa.y, wa.z, kParticleShapeColor});
        lines.push_back({wb.x, wb.y, wb.z, kParticleShapeColor});
    }
    submitLines(lines, lineLayout_, lineProgram_, view);
}

} // namespace kast
