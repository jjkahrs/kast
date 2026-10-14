#pragma once

#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>
#include <vector>

namespace kast {

// ponytail: forward shading with a fixed uniform array per draw; clustered lighting if scenes need more per mesh.
constexpr size_t kMaxPointLights = 8; // MAX_POINT_LIGHTS in fs_mesh.sc

// A PointLight in world space, as the renderer uses it.
struct PointLightSample {
    bx::Vec3 position;
    float range;
    bx::Vec3 color; // color * intensity
};

// Windowed inverse square (Unreal / Frostbite style): saturate(1 - (d/r)^4)^2 / (d^2 + 1). Largest at the light, falls
// smoothly to exactly 0 at distance >= range. fs_mesh.sc evaluates the same formula per pixel.
float pointLightFalloff(float distance, float range);

// Indices of the lights (at most kMaxPointLights) whose range reaches the world-space box, nearest to its center
// first; ties keep list order, the rest are dropped.
std::vector<size_t> nearestPointLights(const std::vector<PointLightSample>& lights, const Aabb& bounds);

// Draws a Scene's MeshRenderers lit by its ambient, its first DirectionalLight and up to kMaxPointLights PointLights
// each, plus editor overlays: a ground grid, axis lines, a short frustum preview per Camera and the range of the
// selected PointLight. The caller owns the view's framebuffer, rect and clear; render() sets the
// view transform and submits.
class Renderer {
public:
    void init();
    void shutdown();
    // overlays = false draws the scene only, as the game sees it. selected: the editor's selection, for its overlays.
    void render(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx, bool overlays = true,
                entt::entity selected = entt::null);

private:
    struct Mesh {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };

    void drawGrid(bgfx::ViewId view);
    void drawCameraFrustums(const Scene& scene, bgfx::ViewId view);
    void drawPointLightRange(const Scene& scene, bgfx::ViewId view, entt::entity selected);

    std::array<Mesh, 3> meshes_; // indexed by Primitive
    bgfx::VertexLayout lineLayout_;
    bgfx::VertexBufferHandle grid_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle meshProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle lineProgram_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle normalMatrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle baseColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightDir_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle ambient_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle pointPosRange_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle pointColor_ = BGFX_INVALID_HANDLE;
};

} // namespace kast
