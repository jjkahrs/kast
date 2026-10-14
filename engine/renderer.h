#pragma once

#include "material.h"
#include "model.h"
#include "scene.h"
#include "shadow.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kast {

// ponytail: forward shading with a fixed uniform array per draw; clustered lighting if scenes need more per mesh.
constexpr size_t kMaxPointLights = 8; // MAX_POINT_LIGHTS in fs_mesh.sc
constexpr size_t kMaxShadowedPointLights = 4; // s_pointShadow0..3 in fs_mesh.sc

// A PointLight in world space, as the renderer uses it.
struct PointLightSample {
    bx::Vec3 position;
    float range;
    bx::Vec3 color; // color * intensity
    bool castShadows = false;
    float bias = 0.0f;
};

// Windowed inverse square (Unreal / Frostbite style): saturate(1 - (d/r)^4)^2 / (d^2 + 1). Largest at the light, falls
// smoothly to exactly 0 at distance >= range. fs_mesh.sc evaluates the same formula per pixel.
float pointLightFalloff(float distance, float range);

// Indices of the lights (at most kMaxPointLights) whose range reaches the world-space box, nearest to its center
// first; ties keep list order, the rest are dropped.
std::vector<size_t> nearestPointLights(const std::vector<PointLightSample>& lights, const Aabb& bounds);

// Indices of the castShadows lights (at most kMaxShadowedPointLights) nearest eye, nearest first; ties keep list order.
std::vector<size_t> shadowedPointLights(const std::vector<PointLightSample>& lights, const bx::Vec3& eye);

// An RGBA8 image with its full mip chain, level 0 first, each level half the last (at least 1) down to 1x1.
struct MipImage {
    uint16_t width = 0, height = 0;
    uint8_t levels = 0;
    std::vector<uint8_t> rgba;
};
// Decodes PNG or JPG bytes and builds the mips (filtered in linear space if srgb). nullopt if it can't be decoded.
std::optional<MipImage> decodeImage(std::string_view bytes, bool srgb);

// Draws a Scene's MeshRenderers and ModelRenderers (posed by their AnimatorState, else in bind pose; skinned on the GPU)
// with their materials (PBR metallic-roughness, see fs_mesh.sc) lit by its ambient, its
// first DirectionalLight and up to kMaxPointLights PointLights each, fogged by its postProcessing.fog, as linear HDR
// color (for PostProcess). Opaque and mask meshes first, then blended ones far to near. Shadows: cascaded shadow maps
// for that DirectionalLight and cube shadow maps for the kMaxShadowedPointLights castShadows PointLights nearest the
// eye, rendered in kShadowViews views before the scene's (see shadow.h). renderOverlays draws the
// editor's: a ground grid, axis lines, a short frustum preview per Camera and the range of the selected PointLight.
// The caller owns the views' framebuffers, rects and clears; these set the view transform and mode and submit.
class Renderer {
public:
    // The cascades, then 6 cube faces per shadowed point light.
    static constexpr bgfx::ViewId kShadowViews = kMaxShadowCascades + kMaxShadowedPointLights * 6;

    void init();
    void shutdown();
    // Materials and textures load from projectRoot (empty = none: every material path renders as an error). Files
    // changed on disk since loaded reload first. shadowViews: the first of kShadowViews views, all before view.
    void render(const Scene& scene, const std::filesystem::path& projectRoot, bgfx::ViewId view, const float* viewMtx,
                const float* projMtx, bgfx::ViewId shadowViews);
    // Into an already encoded (sRGB) target after post, depth-tested against the scene's depth. selected: the
    // editor's selection.
    void renderOverlays(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx,
                        entt::entity selected);

private:
    struct Mesh {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };
    struct ModelMeshBuffers {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };
    // GPU copy of a Model, rebuilt when the library hands out a different (reloaded) one.
    struct GpuModel {
        std::shared_ptr<const Model> source;
        std::vector<ModelMeshBuffers> meshes; // per Model::meshes
        std::vector<Mat4> bindGlobals;        // node globals in bind pose, for models without an AnimatorState
    };
    // One mesh to submit: the transform, buffers and material, plus its bone matrices when skinned.
    struct DrawItem {
        Mat4 world{};
        Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world space, for point lights and blend sorting
        const Material* material = nullptr;
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        int bones = -1; // index into the frame's bone sets; -1 = not skinned
        bool castShadows = true;
        bool receiveShadows = true;
    };
    // Per-frame shadow state the scene draws sample.
    struct ShadowFrame {
        float csmMtx[kMaxShadowCascades][16] = {}; // world to (atlas u, v, depth) per cascade
        float csmSplits[4] = {};                   // far view depth of each cascade (unused: the last)
        float csmBias[kMaxShadowCascades][4] = {}; // x: normal offset meters, y: depth bias
        bool sun = false;                          // the DirectionalLight casts shadows
        std::vector<size_t> pointLights;           // shadowed, by cube index: indices into the frame's point lights
    };
    struct Texture {
        bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; // invalid: failed to load, white_ draws instead
        std::optional<std::filesystem::file_time_type> mtime;
    };

    void setProjectRoot(const std::filesystem::path& root);
    bgfx::TextureHandle texture(const std::string& path, bool srgb); // white_ for none or a failed load
    void loadTexture(const std::string& path, bool srgb, Texture& texture);
    void clearTextures();
    const GpuModel* gpuModel(const std::string& path); // null if the model fails to load
    void clearModels();
    // Appends the draws of a ModelRenderer entity (an error cube if its model fails to load).
    void addModelDraws(const Scene& scene, entt::entity entity, const ModelRenderer& renderer, const float* world,
                       std::vector<DrawItem>& draws, std::vector<std::vector<Mat4>>& boneSets);
    // Renders the shadow maps for this frame's casters into views from shadowViews (creating the maps on first use).
    // sun: the scene's DirectionalLight (null: none), shining along sunDir.
    ShadowFrame renderShadows(const DirectionalLight* sun, const bx::Vec3& sunDir, const std::vector<DrawItem>& draws,
                              const std::vector<std::vector<Mat4>>& boneSets, const std::vector<PointLightSample>& pointLights,
                              const float* viewMtx, const float* projMtx, bgfx::ViewId shadowViews);
    void submitShadowCaster(const DrawItem& item, const std::vector<std::vector<Mat4>>& boneSets, bgfx::ViewId view);
    void drawGrid(bgfx::ViewId view);
    void drawCameraFrustums(const Scene& scene, bgfx::ViewId view);
    void drawPointLightRange(const Scene& scene, bgfx::ViewId view, entt::entity selected);

    std::array<Mesh, 3> meshes_; // indexed by Primitive
    bgfx::VertexLayout lineLayout_;
    bgfx::VertexBufferHandle grid_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout modelLayout_; // ModelVertex
    bgfx::ProgramHandle meshProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle skinnedProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle lineProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle shadowProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle shadowSkinnedProgram_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle normalMatrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle bones_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle baseColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightDir_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle ambient_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle pointPosRange_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle pointColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle emissive_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle materialParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cameraPos_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle fogColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle fogParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle baseColorSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle metallicRoughnessSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmMtx_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSplits_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmBias_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadowParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle pointShadow_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSampler_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::UniformHandle, kMaxShadowedPointLights> pointShadowSamplers_;
    // Created on first use: a 2x2 atlas of kCascadeSize cascades, and per shadowed point light a cube with a frame
    // buffer per face.
    bgfx::TextureHandle csm_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle csmFrameBuffer_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::TextureHandle, kMaxShadowedPointLights> cubes_;
    std::array<std::array<bgfx::FrameBufferHandle, 6>, kMaxShadowedPointLights> cubeFrameBuffers_;
    // 1x1 stand-ins bound to unused shadow stages: D3D11 requires a comparison sampler where the shader declares one.
    bgfx::TextureHandle noShadow2D_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle noShadowCube_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_ = BGFX_INVALID_HANDLE; // 1x1: stands in for no texture and failed loads

    std::filesystem::path root_;
    MaterialLibrary materials_;
    std::map<std::pair<std::string, bool>, Texture> textures_; // by (path, srgb)
    std::map<std::string, GpuModel> models_;                   // by project-relative path
};

} // namespace kast
