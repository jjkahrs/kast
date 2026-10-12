#include "renderer.h"

#include "fs_line.bin.h"
#include "fs_mesh.bin.h"
#include "vs_line.bin.h"
#include "vs_mesh.bin.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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
constexpr int kSphereStacks = 16;
constexpr int kSphereSlices = 32;
constexpr uint64_t kState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                            BGFX_STATE_MSAA; // no back-face culling: an open plane stays visible from below

struct MeshVertex {
    float x, y, z;
    float nx, ny, nz;
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
        mesh.vertices.push_back({p.x, p.y, p.z, normal.x, normal.y, normal.z});
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
            mesh.vertices.push_back({n.x * 0.5f, n.y * 0.5f, n.z * 0.5f, n.x, n.y, n.z});
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

} // namespace

void Renderer::init()
{
    bgfx::VertexLayout meshLayout;
    meshLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .end();
    const MeshData data[3] = {makeCube(), makePlane(), makeSphere()}; // Primitive order
    for (size_t i = 0; i < meshes_.size(); ++i) {
        meshes_[i].vertices = bgfx::createVertexBuffer(copyOf(data[i].vertices), meshLayout);
        meshes_[i].indices = bgfx::createIndexBuffer(copyOf(data[i].indices));
    }

    lineLayout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    grid_ = bgfx::createVertexBuffer(copyOf(makeGrid()), lineLayout_);

    meshProgram_ = makeProgram(vs_mesh, sizeof(vs_mesh), fs_mesh, sizeof(fs_mesh));
    lineProgram_ = makeProgram(vs_line, sizeof(vs_line), fs_line, sizeof(fs_line));
    normalMatrix_ = bgfx::createUniform("u_normalMatrix", bgfx::UniformType::Mat4);
    baseColor_ = bgfx::createUniform("u_baseColor", bgfx::UniformType::Vec4);
    lightDir_ = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);
    lightColor_ = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
}

void Renderer::shutdown()
{
    for (Mesh& mesh : meshes_) {
        bgfx::destroy(mesh.vertices);
        bgfx::destroy(mesh.indices);
    }
    bgfx::destroy(grid_);
    bgfx::destroy(meshProgram_);
    bgfx::destroy(lineProgram_);
    for (bgfx::UniformHandle uniform : {normalMatrix_, baseColor_, lightDir_, lightColor_})
        bgfx::destroy(uniform);
}

void Renderer::render(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx, bool overlays)
{
    bgfx::setViewTransform(view, viewMtx, projMtx);
    const entt::registry& registry = scene.registry();

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

    // ponytail: world matrices are recomputed up the parent chain per entity; cache them when scenes get deep or large.
    for (auto [entity, mesh] : registry.view<MeshRenderer>().each()) {
        float world[16];
        float inverse[16];
        float normalMatrix[16];
        scene.worldMatrix(entity, world);
        bx::mtxInverse(inverse, world);
        bx::mtxTranspose(normalMatrix, inverse);
        const float color[4] = {mesh.color.x, mesh.color.y, mesh.color.z, 1.0f};

        bgfx::setTransform(world);
        bgfx::setUniform(normalMatrix_, normalMatrix);
        bgfx::setUniform(baseColor_, color);
        bgfx::setUniform(lightDir_, lightDir);
        bgfx::setUniform(lightColor_, lightColor);
        const Mesh& gpu = meshes_[static_cast<size_t>(mesh.primitive)];
        bgfx::setVertexBuffer(0, gpu.vertices);
        bgfx::setIndexBuffer(gpu.indices);
        bgfx::setState(kState);
        bgfx::submit(view, meshProgram_);
    }
    if (overlays) {
        drawGrid(view);
        drawCameraFrustums(scene, view);
    }
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
    const auto count = static_cast<uint32_t>(lines.size());
    if (count == 0 || bgfx::getAvailTransientVertexBuffer(count, lineLayout_) < count)
        return;
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, count, lineLayout_);
    std::memcpy(buffer.data, lines.data(), count * sizeof(LineVertex));
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::setState(kState | BGFX_STATE_PT_LINES);
    bgfx::submit(view, lineProgram_);
}

} // namespace kast
