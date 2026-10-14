#include "renderer.h"

#include "fs_line.bin.h"
#include "fs_mesh.bin.h"
#include "vs_line.bin.h"
#include "vs_mesh.bin.h"
#include "vs_mesh_skinned.bin.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

#include "animation.h"
#include "files.h"
#include "log.h"
#include "project.h"

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
constexpr uint32_t kPointLightRangeColor = 0xff40d0f0;
constexpr int kRangeCircleSegments = 48;
constexpr int kSphereStacks = 16;
constexpr int kSphereSlices = 32;
constexpr uint64_t kState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                            BGFX_STATE_MSAA; // no back-face culling: an open plane stays visible from below
// Blended meshes test depth but don't write it, so blended meshes behind them still draw.
constexpr uint64_t kBlendState = (kState & ~BGFX_STATE_WRITE_Z) | BGFX_STATE_BLEND_ALPHA;

struct MeshVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v; // (0, 0) is the image's top-left
};

struct LineVertex {
    float x, y, z;
    uint32_t abgr;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint16_t> indices;
};

// Quad centered at center, spanning +-u and +-v, facing normal = u x v (counter-clockwise from the front).
void addQuad(MeshData& mesh, bx::Vec3 center, bx::Vec3 u, bx::Vec3 v, bx::Vec3 normal)
{
    const auto base = static_cast<uint16_t>(mesh.vertices.size());
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& c : corners) {
        const bx::Vec3 p = bx::add(center, bx::add(bx::mul(u, c[0]), bx::mul(v, c[1])));
        mesh.vertices.push_back({p.x, p.y, p.z, normal.x, normal.y, normal.z, c[0] * 0.5f + 0.5f, 0.5f - c[1] * 0.5f});
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
            mesh.vertices.push_back({n.x * 0.5f, n.y * 0.5f, n.z * 0.5f, n.x, n.y, n.z,
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

bgfx::ProgramHandle makeProgram(const uint8_t* vs, uint32_t vsSize, const uint8_t* fs, uint32_t fsSize)
{
    return bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs, vsSize)), bgfx::createShader(bgfx::makeRef(fs, fsSize)),
                               true);
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

// The file a texture path reads: a model's embedded image "<model>#<i>" comes from the model file.
std::string textureFile(const std::string& path)
{
    const std::string model = embeddedTextureModel(path);
    return model.empty() ? path : model;
}

} // namespace

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
    // Each level is filtered from the one before; sRGB images are filtered in linear space so the mips don't darken.
    size_t level = 0; // byte offset of the last level
    for (int w = width, h = height; w > 1 || h > 1; ++image.levels) {
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
    return image;
}

float pointLightFalloff(float distance, float range)
{
    const float ratio = distance / range;
    const float window = bx::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
    return window * window / (distance * distance + 1.0f);
}

std::vector<size_t> nearestPointLights(const std::vector<PointLightSample>& lights, const Aabb& bounds)
{
    const bx::Vec3 center = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    std::vector<std::pair<float, size_t>> reaching; // (squared distance to center, index)
    for (size_t i = 0; i < lights.size(); ++i) {
        const PointLightSample& light = lights[i];
        const bx::Vec3 closest = bx::clamp(light.position, bounds.min, bounds.max);
        if (bx::length(bx::sub(light.position, closest)) < light.range)
            reaching.emplace_back(bx::distanceSq(light.position, center), i);
    }
    const size_t count = std::min(reaching.size(), kMaxPointLights);
    std::partial_sort(reaching.begin(), reaching.begin() + static_cast<ptrdiff_t>(count), reaching.end());
    std::vector<size_t> indices;
    for (size_t i = 0; i < count; ++i)
        indices.push_back(reaching[i].second);
    return indices;
}

void Renderer::init()
{
    bgfx::VertexLayout meshLayout;
    meshLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    const MeshData data[3] = {makeCube(), makePlane(), makeSphere()}; // Primitive order
    for (size_t i = 0; i < meshes_.size(); ++i) {
        meshes_[i].vertices = bgfx::createVertexBuffer(copyOf(data[i].vertices), meshLayout);
        meshes_[i].indices = bgfx::createIndexBuffer(copyOf(data[i].indices));
    }

    modelLayout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Indices, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Weight, 4, bgfx::AttribType::Float)
        .end();
    static_assert(sizeof(ModelVertex) == 16 * sizeof(float), "ModelVertex must match modelLayout_");

    lineLayout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    grid_ = bgfx::createVertexBuffer(copyOf(makeGrid()), lineLayout_);

    meshProgram_ = makeProgram(vs_mesh, sizeof(vs_mesh), fs_mesh, sizeof(fs_mesh));
    skinnedProgram_ = makeProgram(vs_mesh_skinned, sizeof(vs_mesh_skinned), fs_mesh, sizeof(fs_mesh));
    lineProgram_ = makeProgram(vs_line, sizeof(vs_line), fs_line, sizeof(fs_line));
    normalMatrix_ = bgfx::createUniform("u_normalMatrix", bgfx::UniformType::Mat4);
    bones_ = bgfx::createUniform("u_bones", bgfx::UniformType::Mat4, static_cast<uint16_t>(kMaxBones));
    baseColor_ = bgfx::createUniform("u_baseColor", bgfx::UniformType::Vec4);
    lightDir_ = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);
    lightColor_ = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
    ambient_ = bgfx::createUniform("u_ambient", bgfx::UniformType::Vec4);
    pointPosRange_ = bgfx::createUniform("u_pointPosRange", bgfx::UniformType::Vec4, kMaxPointLights);
    pointColor_ = bgfx::createUniform("u_pointColor", bgfx::UniformType::Vec4, kMaxPointLights);
    emissive_ = bgfx::createUniform("u_emissive", bgfx::UniformType::Vec4);
    materialParams_ = bgfx::createUniform("u_material", bgfx::UniformType::Vec4);
    cameraPos_ = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);
    fogColor_ = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);
    fogParams_ = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4);
    baseColorSampler_ = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
    metallicRoughnessSampler_ = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
    const uint32_t white = 0xffffffff;
    white_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE,
                                   bgfx::copy(&white, sizeof(white)));
}

void Renderer::shutdown()
{
    for (Mesh& mesh : meshes_) {
        bgfx::destroy(mesh.vertices);
        bgfx::destroy(mesh.indices);
    }
    bgfx::destroy(grid_);
    bgfx::destroy(meshProgram_);
    bgfx::destroy(skinnedProgram_);
    bgfx::destroy(lineProgram_);
    for (bgfx::UniformHandle uniform : {normalMatrix_, bones_, baseColor_, lightDir_, lightColor_, ambient_, pointPosRange_,
                                        pointColor_, emissive_, materialParams_, cameraPos_, fogColor_, fogParams_,
                                        baseColorSampler_, metallicRoughnessSampler_})
        bgfx::destroy(uniform);
    clearTextures();
    clearModels();
    bgfx::destroy(white_);
}

void Renderer::setProjectRoot(const std::filesystem::path& root)
{
    if (root != root_) {
        root_ = root;
        clearTextures();
        clearModels();
    }
    materials_.setRoot(root);
    materials_.reloadChanged();
    models().setRoot(root);
    models().reloadChanged();
    // ponytail: one stat per texture per frame, as for materials; watch the folder if projects grow to thousands.
    for (auto& [key, texture] : textures_)
        if (!root_.empty() && modifiedTime(root_ / textureFile(key.first)) != texture.mtime)
            loadTexture(key.first, key.second, texture);
}

bgfx::TextureHandle Renderer::texture(const std::string& path, bool srgb)
{
    if (path.empty())
        return white_;
    auto [it, added] = textures_.try_emplace({path, srgb});
    if (added)
        loadTexture(path, srgb, it->second);
    return bgfx::isValid(it->second.handle) ? it->second.handle : white_;
}

void Renderer::loadTexture(const std::string& path, bool srgb, Texture& texture)
{
    if (bgfx::isValid(texture.handle))
        bgfx::destroy(texture.handle);
    texture.handle = BGFX_INVALID_HANDLE;
    std::string error = "no project";
    const std::optional<std::filesystem::path> file =
        root_.empty() ? std::nullopt : resolveProjectPath(root_, textureFile(path), error);
    texture.mtime = file ? modifiedTime(*file) : std::nullopt;
    std::string bytes;
    std::optional<MipImage> image;
    if (!embeddedTextureModel(path).empty()) { // embedded in a model
        const std::optional<std::string_view> embedded = models().embeddedTexture(path);
        if (embedded)
            bytes = *embedded;
        else
            error = "no such embedded image";
        if (embedded && !(image = decodeImage(bytes, srgb)))
            error = std::format("cannot decode it as PNG or JPG ({})", stbi_failure_reason());
    } else if (file && !readFile(*file, bytes)) {
        error = "cannot read the file";
    } else if (file && !(image = decodeImage(bytes, srgb))) {
        error = std::format("cannot decode it as PNG or JPG ({})", stbi_failure_reason());
    }
    if (!image) {
        warnOnce(std::format("Texture '{}': {}; using white", path, error));
        return;
    }
    texture.handle = bgfx::createTexture2D(image->width, image->height, image->levels > 1, 1, bgfx::TextureFormat::RGBA8,
                                           srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE,
                                           bgfx::copy(image->rgba.data(), static_cast<uint32_t>(image->rgba.size())));
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

void Renderer::addModelDraws(const Scene& scene, entt::entity entity, const ModelRenderer& renderer, const float* world,
                             std::vector<DrawItem>& draws, std::vector<std::vector<Mat4>>& boneSets)
{
    if (renderer.model.empty())
        return;
    DrawItem item;
    std::copy_n(world, 16, item.world.begin());
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
        draws.push_back(item);
    }
}

void Renderer::render(const Scene& scene, const std::filesystem::path& projectRoot, bgfx::ViewId view,
                      const float* viewMtx, const float* projMtx)
{
    setProjectRoot(projectRoot);
    bgfx::setViewTransform(view, viewMtx, projMtx);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential); // blended meshes must draw in the order submitted
    const entt::registry& registry = scene.registry();
    float cameraWorld[16];
    bx::mtxInverse(cameraWorld, viewMtx);
    const bx::Vec3 eye{cameraWorld[12], cameraWorld[13], cameraWorld[14]};
    const float cameraPos[4] = {eye.x, eye.y, eye.z, 1.0f};

    float lightDir[4] = {0.0f, -1.0f, 0.0f, 0.0f};
    float lightColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool lightFound = false;
    scene.forEachDepthFirst([&](entt::entity entity) {
        const auto* light = registry.try_get<DirectionalLight>(entity);
        if (lightFound || !light)
            return;
        lightFound = true;
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
    });
    const SceneLighting& lighting = scene.lighting;
    const float ambient[4] = {lighting.ambientColor.x * lighting.ambientIntensity,
                              lighting.ambientColor.y * lighting.ambientIntensity,
                              lighting.ambientColor.z * lighting.ambientIntensity, 0.0f};
    const ScenePostProcessing::Fog& fog = scene.postProcessing.fog;
    const float fogColor[4] = {fog.color.x, fog.color.y, fog.color.z, fog.enabled ? 1.0f : 0.0f};
    const float fogParams[4] = {fog.density, fog.start, 0.0f, 0.0f};

    std::vector<PointLightSample> pointLights;
    for (auto [entity, light] : registry.view<PointLight>().each()) {
        float world[16];
        scene.worldMatrix(entity, world);
        pointLights.push_back({{world[12], world[13], world[14]}, light.range, bx::mul(light.color, light.intensity)});
    }

    // ponytail: world matrices are recomputed up the parent chain per entity; cache them when scenes get deep or large.
    std::vector<DrawItem> draws;
    std::vector<std::vector<Mat4>> boneSets;
    for (auto [entity, mesh] : registry.view<MeshRenderer>().each()) {
        const auto* instance = registry.try_get<MaterialInstance>(entity);
        const Mesh& gpu = meshes_[static_cast<size_t>(mesh.primitive)];
        DrawItem item;
        scene.worldMatrix(entity, item.world.data());
        item.bounds = transformBounds(primitiveBounds(mesh.primitive), item.world.data());
        item.material = instance ? &instance->material : &materials_.get(mesh.material);
        item.vertices = gpu.vertices;
        item.indices = gpu.indices;
        draws.push_back(item);
    }
    for (auto [entity, model] : registry.view<ModelRenderer>().each()) {
        float world[16];
        scene.worldMatrix(entity, world);
        addModelDraws(scene, entity, model, world, draws, boneSets);
    }

    const auto submit = [&](const DrawItem& item) {
        const Material& material = *item.material;
        float inverse[16];
        float normalMatrix[16];
        bx::mtxInverse(inverse, item.world.data());
        bx::mtxTranspose(normalMatrix, inverse);
        // Unused slots: black, range 1, so they add nothing.
        float pointPosRange[kMaxPointLights][4] = {};
        float pointColor[kMaxPointLights][4] = {};
        for (auto& slot : pointPosRange)
            slot[3] = 1.0f;
        const std::vector<size_t> nearest = nearestPointLights(pointLights, item.bounds);
        for (size_t slot = 0; slot < nearest.size(); ++slot) {
            const PointLightSample& light = pointLights[nearest[slot]];
            std::memcpy(pointPosRange[slot], &light.position, sizeof(bx::Vec3));
            pointPosRange[slot][3] = light.range;
            std::memcpy(pointColor[slot], &light.color, sizeof(bx::Vec3));
        }
        const bx::Vec3 radiance = emissiveRadiance(material);
        const float emissive[4] = {radiance.x, radiance.y, radiance.z, 0.0f};
        const bool blend = material.alphaMode == AlphaMode::Blend;
        // z: fragments with alpha below it are discarded (-1 = none); w: 1 outputs alpha (blend), 0 outputs 1.
        const float params[4] = {material.metallic, material.roughness,
                                 material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : -1.0f, blend ? 1.0f : 0.0f};

        bgfx::setTransform(item.world.data());
        bgfx::setUniform(normalMatrix_, normalMatrix);
        if (item.bones >= 0) {
            const std::vector<Mat4>& bones = boneSets[static_cast<size_t>(item.bones)];
            bgfx::setUniform(bones_, bones.data(), static_cast<uint16_t>(bones.size()));
        }
        bgfx::setUniform(baseColor_, material.baseColor.data());
        bgfx::setUniform(emissive_, emissive);
        bgfx::setUniform(materialParams_, params);
        bgfx::setUniform(cameraPos_, cameraPos);
        bgfx::setUniform(fogColor_, fogColor);
        bgfx::setUniform(fogParams_, fogParams);
        bgfx::setUniform(lightDir_, lightDir);
        bgfx::setUniform(lightColor_, lightColor);
        bgfx::setUniform(ambient_, ambient);
        bgfx::setUniform(pointPosRange_, pointPosRange, kMaxPointLights);
        bgfx::setUniform(pointColor_, pointColor, kMaxPointLights);
        bgfx::setTexture(0, baseColorSampler_, texture(material.baseColorTexture, true));
        bgfx::setTexture(1, metallicRoughnessSampler_, texture(material.metallicRoughnessTexture, false));
        bgfx::setVertexBuffer(0, item.vertices);
        bgfx::setIndexBuffer(item.indices);
        bgfx::setState(blend ? kBlendState : kState);
        bgfx::submit(view, item.bones >= 0 ? skinnedProgram_ : meshProgram_);
    };

    // Opaque and mask meshes now; blended ones after, far to near by world-space bounds center.
    std::vector<size_t> blended;
    std::vector<bx::Vec3> centers;
    for (size_t i = 0; i < draws.size(); ++i) {
        if (draws[i].material->alphaMode != AlphaMode::Blend) {
            submit(draws[i]);
            continue;
        }
        blended.push_back(i);
        centers.push_back(bx::mul(bx::add(draws[i].bounds.min, draws[i].bounds.max), 0.5f));
    }
    for (size_t i : backToFront(centers, eye))
        submit(draws[blended[i]]);
}

void Renderer::renderOverlays(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx,
                              entt::entity selected)
{
    bgfx::setViewTransform(view, viewMtx, projMtx);
    // ponytail: blended meshes don't write depth, so overlays draw over glass; draw them before blended meshes in
    // a separate HDR pass if that matters.
    drawGrid(view);
    drawCameraFrustums(scene, view);
    drawPointLightRange(scene, view, selected);
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

// Three axis-aligned circles of radius range around the selected PointLight (world position; scale ignored).
void Renderer::drawPointLightRange(const Scene& scene, bgfx::ViewId view, entt::entity selected)
{
    const auto* light = scene.valid(selected) ? scene.registry().try_get<PointLight>(selected) : nullptr;
    if (!light)
        return;
    float world[16];
    scene.worldMatrix(selected, world);
    const bx::Vec3 center{world[12], world[13], world[14]};
    std::vector<LineVertex> lines;
    for (int axis = 0; axis < 3; ++axis) {
        const auto point = [&](int segment) {
            const float angle = 2.0f * bx::kPi * static_cast<float>(segment) / kRangeCircleSegments;
            float offset[3] = {};
            offset[(axis + 1) % 3] = std::cos(angle) * light->range;
            offset[(axis + 2) % 3] = std::sin(angle) * light->range;
            return LineVertex{center.x + offset[0], center.y + offset[1], center.z + offset[2], kPointLightRangeColor};
        };
        for (int segment = 0; segment < kRangeCircleSegments; ++segment) {
            lines.push_back(point(segment));
            lines.push_back(point(segment + 1));
        }
    }
    submitLines(lines, lineLayout_, lineProgram_, view);
}

} // namespace kast
