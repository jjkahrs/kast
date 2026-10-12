#pragma once

#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>

namespace kast {

// Draws a Scene's MeshRenderers lit by its first DirectionalLight, plus editor overlays: a ground grid, axis lines
// and a short frustum preview per Camera. The caller owns the view's framebuffer, rect and clear; render() sets the
// view transform and submits.
class Renderer {
public:
    void init();
    void shutdown();
    // overlays = false draws the scene only, as the game sees it.
    void render(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx, bool overlays = true);

private:
    struct Mesh {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };

    void drawGrid(bgfx::ViewId view);
    void drawCameraFrustums(const Scene& scene, bgfx::ViewId view);

    std::array<Mesh, 3> meshes_; // indexed by Primitive
    bgfx::VertexLayout lineLayout_;
    bgfx::VertexBufferHandle grid_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle meshProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle lineProgram_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle normalMatrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle baseColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightDir_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightColor_ = BGFX_INVALID_HANDLE;
};

} // namespace kast
