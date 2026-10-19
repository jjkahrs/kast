#pragma once

#include "frustum.h"
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
// Appends the mips below level 0 (image.rgba holds just width x height RGBA8) and sets levels.
void buildMips(MipImage& image, bool srgb);

// Draws a Scene's MeshRenderers and ModelRenderers (posed by their AnimatorState, else in bind pose; skinned on the GPU)
// with their materials (PBR metallic-roughness, see fs_mesh.sc) lit by its ambient, its
// first DirectionalLight and up to kMaxPointLights PointLights each, fogged by its postProcessing.fog, as linear HDR
// color (for PostProcess). Opaque and mask meshes first, then blended ones far to near. Shadows: cascaded shadow maps
// for that DirectionalLight and cube shadow maps for the kMaxShadowedPointLights castShadows PointLights nearest the
// eye, rendered in kShadowViews views before the scene's (see shadow.h). renderOverlays draws the
// editor's: a ground grid, axis lines, a short frustum preview per Camera, the range of the selected PointLight and
// the selected entity's collider.
// Every pass culls its draws to its view frustum (main camera, each cascade, each cube face), and draws that share
// buffers, material, shadow flags and (main pass) point lights, unskinned and not blended, are drawn as one GPU-instanced
// submit (see docs/engine/instancing-and-culling.html).
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
    // Until replaced (empty materialPath: none), terrain whose map material is materialPath draws with material, and
    // the sRGB texture texturePath is image instead of either file: the zone builder's preview of a material it has
    // not saved yet.
    void setPreview(const std::string& materialPath, const Material& material, const std::string& texturePath,
                    MipImage image);
    // Both on by default. Instancing off: every draw is its own submit. Culling off: every draw goes to every pass.
    // Either way the output is the same, so off is a reference (--no-instancing, --no-culling).
    void setInstancing(bool on) { instancing_ = on; }
    void setCulling(bool on) { culling_ = on; }

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
        Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world space, for culling, point lights and blend sorting
        const Material* material = nullptr;
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        int bones = -1; // index into the frame's bone sets; -1 = not skinned
        bool posed = false; // animated: bounds are the bind pose's, so the draw is never culled
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
    // Builds, rebuilds (new data) and drops the GPU meshes of the scene's TerrainTiles, then appends their draws.
    void addTerrainDraws(const Scene& scene, std::vector<DrawItem>& draws);
    void clearTerrain();
    // Appends the draws of a ModelRenderer entity (an error cube if its model fails to load).
    void addModelDraws(const Scene& scene, entt::entity entity, const ModelRenderer& renderer, const float* world,
                       std::vector<DrawItem>& draws, std::vector<std::vector<Mat4>>& boneSets);
    // Renders the shadow maps for this frame's casters into views from shadowViews (creating the maps on first use).
    // sun: the scene's DirectionalLight (null: none), shining along sunDir.
    ShadowFrame renderShadows(const DirectionalLight* sun, const bx::Vec3& sunDir, const std::vector<DrawItem>& draws,
                              const std::vector<std::vector<Mat4>>& boneSets, const std::vector<PointLightSample>& pointLights,
                              const float* viewMtx, const float* projMtx, bgfx::ViewId shadowViews);
    // Draws one batch (see batchDraws) into a shadow view: instanced if it can be, else one by one.
    void submitShadowCasters(const std::vector<const DrawItem*>& batch, const std::vector<std::vector<Mat4>>& boneSets,
                             bgfx::ViewId view);
    // Culling is on and item is outside frustum (posed draws never are: their bounds are the bind pose's).
    bool culled(const DrawItem& item, const Frustum& frustum) const
    {
        return culling_ && !item.posed && !intersects(frustum, item.bounds);
    }
    // True if batch can be one instanced submit, with its world matrices in instances (this frame's buffer).
    bool instanced(const std::vector<const DrawItem*>& batch, bgfx::InstanceDataBuffer& instances) const;
    void drawGrid(bgfx::ViewId view);
    void drawCameraFrustums(const Scene& scene, bgfx::ViewId view);
    void drawPointLightRange(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawColliderWireframe(const Scene& scene, bgfx::ViewId view, entt::entity selected);

    // GPU copy of a TerrainTile's heights: kTileSamples^2 vertices in tile-local metres; indices are terrainIndices_.
    struct TerrainMesh {
        std::shared_ptr<const TerrainTileData> data; // rebuilt when the tile's data changes
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world space
    };

    std::array<Mesh, 3> meshes_; // indexed by Primitive
    bgfx::VertexLayout meshLayout_;  // MeshVertex: primitives and terrain
    bgfx::IndexBufferHandle terrainIndices_ = BGFX_INVALID_HANDLE; // 32-bit, map::for_each_triangle, shared by tiles
    std::map<entt::entity, TerrainMesh> terrain_;
    bgfx::VertexLayout lineLayout_;
    bgfx::VertexBufferHandle grid_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout modelLayout_; // ModelVertex
    bgfx::ProgramHandle meshProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle skinnedProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle instancedProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle lineProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle shadowProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle shadowSkinnedProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle shadowInstancedProgram_ = BGFX_INVALID_HANDLE;
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
    std::string previewMaterialPath_, previewTexturePath_;      // setPreview
    Material previewMaterial_;
    bgfx::TextureHandle previewTexture_ = BGFX_INVALID_HANDLE;
    bool instancing_ = true;
    bool culling_ = true;
};

} // namespace kast
