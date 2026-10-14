#pragma once

#include "material.h"
#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

// An RGBA8 image with its full mip chain, level 0 first, each level half the last (at least 1) down to 1x1.
struct MipImage {
    uint16_t width = 0, height = 0;
    uint8_t levels = 0;
    std::vector<uint8_t> rgba;
};
// Decodes PNG or JPG bytes and builds the mips (filtered in linear space if srgb). nullopt if it can't be decoded.
std::optional<MipImage> decodeImage(std::string_view bytes, bool srgb);

// Draws a Scene's MeshRenderers with their materials (PBR metallic-roughness, see fs_mesh.sc) lit by its ambient, its
// first DirectionalLight and up to kMaxPointLights PointLights each, plus editor overlays: a ground grid, axis lines, a
// short frustum preview per Camera and the range of the selected PointLight. Opaque and mask meshes first, then blended
// ones far to near. The caller owns the view's framebuffer, rect and clear; render() sets the view transform and
// mode and submits.
class Renderer {
public:
    void init();
    void shutdown();
    // Materials and textures load from projectRoot (empty = none: every material path renders as an error). Files
    // changed on disk since loaded reload first. overlays = false draws the scene only, as the game sees it.
    // selected: the editor's selection, for its overlays.
    void render(const Scene& scene, const std::filesystem::path& projectRoot, bgfx::ViewId view, const float* viewMtx,
                const float* projMtx, bool overlays = true, entt::entity selected = entt::null);

private:
    struct Mesh {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };
    struct Texture {
        bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; // invalid: failed to load, white_ draws instead
        std::optional<std::filesystem::file_time_type> mtime;
    };

    void setProjectRoot(const std::filesystem::path& root);
    bgfx::TextureHandle texture(const std::string& path, bool srgb); // white_ for none or a failed load
    void loadTexture(const std::string& path, bool srgb, Texture& texture);
    void clearTextures();
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
    bgfx::UniformHandle emissive_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle materialParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cameraPos_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle baseColorSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle metallicRoughnessSampler_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_ = BGFX_INVALID_HANDLE; // 1x1: stands in for no texture and failed loads

    std::filesystem::path root_;
    MaterialLibrary materials_;
    std::map<std::pair<std::string, bool>, Texture> textures_; // by (path, srgb)
};

} // namespace kast
