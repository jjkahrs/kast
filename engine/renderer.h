#pragma once

#include "frustum.h"
#include "material.h"
#include "model.h"
#include "particles.h"
#include "scene.h"
#include "shader.h"
#include "shader_clock.h"
#include "shadow.h"
#include "sky.h"
#include "texture_cook.h"

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

// A primitive or terrain vertex (meshLayout_). Tangent: xyz along texture u, w the handedness: the bitangent (image up,
// the normal map's +Y) is cross(normal, tangent) * w.
struct MeshVertex {
    float x, y, z;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v; // (0, 0) is the image's top-left
    uint32_t color = 0xffffffff; // RGBA8 (R in the low byte), linear: COLOR0, white
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint16_t> indices;
};

// The built-in mesh of a MeshRenderer primitive (1 m across, centered), as the renderer draws it.
MeshData primitiveMesh(Primitive primitive);

// The tangent of a terrain vertex with this normal: world +X (texture u) made perpendicular to it. w is 1 (image up is -Z).
inline bx::Vec3 terrainTangent(const bx::Vec3& normal)
{
    return bx::normalize(bx::sub(bx::Vec3{1.0f, 0.0f, 0.0f}, bx::mul(normal, normal.x)));
}

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
// An authored KTX / DDS texture as stored: its GPU format (BCn or uncompressed), size and its own mips.
struct GpuImageInfo {
    bgfx::TextureFormat::Enum format = bgfx::TextureFormat::Unknown;
    uint16_t width = 0, height = 0;
    uint8_t mips = 0;
    bool cube = false;
};
// True if the bytes are a KTX or DDS file (by their magic, so embedded model images count too), or the path ends in
// .ktx / .dds (so a corrupt one still reports as such). Those load with createGpuTexture, the rest with decodeImage.
bool isGpuImage(std::string_view bytes, std::string_view path = {});
// Reads a KTX / DDS header (bimg::imageParse) and checks the data is all there. nullopt and why if it's corrupt,
// truncated, or a 2D texture where a cube is wanted (cube) or the reverse.
std::optional<GpuImageInfo> parseGpuImage(std::string_view bytes, bool cube, std::string& error);
// A texture in the file's stored format with its own mips (srgb: sampled as sRGB, BGFX_TEXTURE_SRGB). Invalid and why
// if parseGpuImage fails or this GPU lacks the format natively (bgfx's CPU emulation is refused: it would decode to
// RGBA8 and lose the point of compression).
bgfx::TextureHandle createGpuTexture(std::string_view bytes, bool srgb, bool cube, uint64_t flags, std::string& error);
// "Asset loads: N models in X ms, M textures in Y ms; textureMemoryUsed Z MiB" (assetLoadStats, bgfx::getStats).
std::string assetLoadSummary();
// Appends the mips below level 0 (image.rgba holds just width x height RGBA8) and sets levels.
void buildMips(MipImage& image, bool srgb);

// Draws a Scene's MeshRenderers and ModelRenderers (posed by their AnimatorState, else in bind pose; skinned on the GPU)
// with their materials (PBR metallic-roughness, see fs_mesh.sc) lit by its ambient, its
// first DirectionalLight and up to kMaxPointLights PointLights each, fogged by its postProcessing.fog, as linear HDR
// color (for PostProcess). Opaque and mask meshes first, then its sky (sky.h), then blended ones and particle emitters
// (particles.h, simulated on the GPU here) far to near. Blended draws whose .kshader samples SCREEN_TEXTURE or
// DEPTH_TEXTURE see a copy of the target's color or depth as the opaque meshes and the sky left it, made only on frames
// such a draw is in view.
// Shadows: cascaded shadow maps for that DirectionalLight and cube shadow maps for the kMaxShadowedPointLights
// castShadows PointLights nearest the eye, rendered in kShadowViews views before the scene's (see shadow.h).
// renderOverlays draws the editor's: a ground grid, axis lines, a short frustum preview per Camera, the range of the
// selected PointLight, the selected entity's collider and its ParticleEmitter's shape.
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
    // changed on disk since loaded reload first. shadowViews: the first of kShadowViews views, all before view. With a
    // target (setTarget) the blended draws go to view + 1, after the SCREEN_TEXTURE / DEPTH_TEXTURE copies.
    // extra: another scene whose meshes, models and terrain draw too, lit by scene's lights, sky and fog (the editor's
    // open zone over the edited scene).
    void render(const Scene& scene, const std::filesystem::path& projectRoot, bgfx::ViewId view, const float* viewMtx,
                const float* projMtx, bgfx::ViewId shadowViews, const Scene* extra = nullptr);
    // Into an already encoded (sRGB) target after post, depth-tested against the scene's depth. selected: the
    // editor's selection.
    void renderOverlays(const Scene& scene, bgfx::ViewId view, const float* viewMtx, const float* projMtx,
                        entt::entity selected);
    // Until replaced (empty materialPath: none), terrain whose map material is materialPath draws with material, and
    // the sRGB texture texturePath is image instead of either file: the editor's zone preview of a material not
    // saved yet (Zone Builder, Create texture & material).
    void setPreview(const std::string& materialPath, const Material& material, const std::string& texturePath,
                    MipImage image);
    // What the next render() draws into with view (PostProcess::target(): RGBA16F color, D24S8 depth) and its size, so
    // it can copy them for SCREEN_TEXTURE / DEPTH_TEXTURE. Set before each render(); without one (the default) blended
    // draws stay in view and those sample black and the far plane.
    void setTarget(bgfx::FrameBufferHandle target, uint16_t width, uint16_t height);
    // Frames whose opaque scene color / depth was copied for SCREEN_TEXTURE / DEPTH_TEXTURE, since init.
    struct SceneCopies {
        uint64_t screen = 0, depth = 0;
    };
    const SceneCopies& sceneCopies() const { return copies_; }
    // u_time.x for this frame's mesh draws (ShaderClock::seconds).
    void setTime(float seconds) { timeSeconds_ = seconds; }
    // Both on by default. Instancing off: every draw is its own submit. Culling off: every draw goes to every pass.
    // Either way the output is the same, so off is a reference (--no-instancing, --no-culling).
    void setInstancing(bool on) { instancing_ = on; }
    void setCulling(bool on) { culling_ = on; }
    // Why the scene's cubemap sky last fell back to the procedural one ("" = it didn't): the Lighting panel shows it.
    const std::string& skyError() const { return sky_.error(); }

private:
    struct Mesh {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };
    struct ModelMeshBuffers {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
    };
    // A .kshader's programs: its fs_mesh (main pass) and its shadow fragment shader (shadow passes), each with its mesh
    // vertex shaders (the built-in ones if it has no vertex()). Rebuilt when the library hands out a different
    // (recompiled) shader.
    struct UserShader {
        std::shared_ptr<const CompiledShader> source;
        std::vector<bgfx::ShaderHandle> shaders; // the ones made from its bytecode (not the built-in vertex shaders)
        bgfx::ProgramHandle mesh = BGFX_INVALID_HANDLE, skinned = BGFX_INVALID_HANDLE, instanced = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle shadowMesh = BGFX_INVALID_HANDLE, shadowSkinned = BGFX_INVALID_HANDLE,
                            shadowInstanced = BGFX_INVALID_HANDLE;
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
        const UserShader* shader = nullptr; // the material's .kshader; null: the built-in mesh shader
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        int bones = -1; // index into the frame's bone sets; -1 = not skinned
        bool posed = false; // animated: bounds are the bind pose's, so the draw is never culled
        bool mirrored = false; // world matrix has a negative determinant: front faces wind the other way
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
        std::shared_future<CookedTexture> cooking; // valid: handle is the RGBA8 image until this cook is in
    };

    void setProjectRoot(const std::filesystem::path& root);
    bgfx::TextureHandle texture(const std::string& path, TextureUsage usage); // white_ for none or a failed load
    bgfx::TextureHandle texture(const std::string& path, TextureUsage usage, bgfx::TextureHandle fallback); // or fallback
    // A PNG / JPG loads as its cooked KTX: from the editor's texture cache (TextureCooks; on a miss the RGBA8 image until
    // the cook is in, see takeCooked), or game.pak's (the player). KTX / DDS files load as stored.
    void loadTexture(const std::string& path, TextureUsage usage, Texture& texture);
    // The GPU texture of a finished cook (invalid, warned once, if it failed: the RGBA8 image stays).
    bgfx::TextureHandle cookedTexture(const std::string& path, TextureUsage usage, const CookedTexture& cooked);
    // The player's: game.pak's cooked KTX of path (another usage's if usage has none); invalid + one error if none.
    bgfx::TextureHandle pakTexture(const std::string& path, TextureUsage usage);
    void clearTextures();
    const GpuModel* gpuModel(const std::string& path); // null if the model fails to load
    void clearModels();
    const UserShader* userShader(const std::string& path); // null if the .kshader fails to load or compile
    void destroy(UserShader& shader);
    // Points each draw whose material has a .kshader at its programs; a shader that fails, or material values of the
    // wrong type for it, draw with errorMaterial() instead.
    void resolveShaders(std::vector<DrawItem>& draws);
    // Builds the GPU meshes of the scene's TerrainTiles (one per TerrainTileData: new data, new mesh), marks them used
    // and appends their draws. pruneTerrain then drops the meshes no rendered scene used this frame.
    void addTerrainDraws(const Scene& scene, std::vector<DrawItem>& draws);
    void pruneTerrain();
    void clearTerrain();
    // Appends the draws of a ModelRenderer entity (an error cube if its model fails to load).
    void addModelDraws(const Scene& scene, entt::entity entity, const ModelRenderer& renderer, const float* world,
                       std::vector<DrawItem>& draws, std::vector<std::vector<Mat4>>& boneSets);
    // Renders the shadow maps for this frame's casters into views from shadowViews (creating the maps on first use).
    // sun: the scene's DirectionalLight (null: none), shining along sunDir.
    ShadowFrame renderShadows(const DirectionalLight* sun, const bx::Vec3& sunDir, const std::vector<DrawItem>& draws,
                              const std::vector<std::vector<Mat4>>& boneSets, const std::vector<PointLightSample>& pointLights,
                              const float* viewMtx, const float* projMtx, bgfx::ViewId shadowViews);
    // Draws one batch (see batchDraws) into a shadow view: instanced if it can be, else one by one. eye: the camera's
    // world position (CAMERA_POSITION for .kshader casters).
    void submitShadowCasters(const std::vector<const DrawItem*>& batch, const std::vector<std::vector<Mat4>>& boneSets,
                             const bx::Vec3& eye, bgfx::ViewId view);
    // The uniforms and textures of item's material (and its .kshader's params) and u_time, as fs_mesh.sc reads them.
    void setMaterialState(const DrawItem& item);
    // Blits the target's color (screen) and depth into screenCopy_ / depthCopy_ at the start of view, before its draws
    // (creating them, or remaking them for a new size, first).
    // ponytail: one copy per frame, of the opaque scene: blended surfaces don't see each other through SCREEN_TEXTURE;
    // copy again before such a draw (as Godot's BackBufferCopy) if layered refraction is ever needed.
    void copyScene(bgfx::ViewId view, bgfx::FrameBufferHandle target, bool screen, bool depth);
    void destroyCopies();
    // Culling is on and item is outside frustum (posed draws never are: their bounds are the bind pose's).
    // ponytail: by the undisplaced bounds; a .kshader vertex() moving geometry outside them can pop at the frustum's edge.
    // Pad the bounds per material (a .kshader "cull margin") if that shows.
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
    void drawParticleShape(const Scene& scene, bgfx::ViewId view, entt::entity selected);

    // GPU copy of a TerrainTile's heights: kTileSamples^2 vertices in tile-local metres; indices are terrainIndices_.
    struct TerrainMesh {
        std::shared_ptr<const TerrainTileData> data; // the key's, kept alive so no other data reuses its address
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world space
        bool used = false; // by a tile this frame
    };

    std::array<Mesh, 3> meshes_; // indexed by Primitive
    bgfx::VertexLayout meshLayout_;  // MeshVertex: primitives and terrain
    bgfx::IndexBufferHandle terrainIndices_ = BGFX_INVALID_HANDLE; // 32-bit, map::for_each_triangle, shared by tiles
    std::map<const TerrainTileData*, TerrainMesh> terrain_; // data is kept alive by its mesh
    bgfx::VertexLayout lineLayout_;
    bgfx::VertexBufferHandle grid_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout modelLayout_; // ModelVertex
    // The built-in mesh vertex shaders, shared by the built-in programs and every .kshader's.
    bgfx::ShaderHandle meshVertex_ = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle skinnedVertex_ = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle instancedVertex_ = BGFX_INVALID_HANDLE;
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
    bgfx::UniformHandle materialExt_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uvTransform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle time_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cameraPos_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle fogColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle fogParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle baseColorSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle metallicRoughnessSampler_ = BGFX_INVALID_HANDLE;
    // Material texture stages 7..9 (s_normal, s_occlusion, s_emissive in fs_mesh.sc): after the shadow samplers on 2..6.
    bgfx::UniformHandle normalSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle occlusionSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle emissiveSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmMtx_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSplits_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmBias_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadowParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle pointShadow_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSampler_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::UniformHandle, kMaxShadowedPointLights> pointShadowSamplers_;
    bgfx::UniformHandle kastParams_ = BGFX_INVALID_HANDLE; // u_kastParams: a .kshader's numeric params
    std::array<bgfx::UniformHandle, kMaxShaderTextures> shaderTextureSamplers_; // s_kastTexture0.., its texture2D params
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
    bgfx::TextureHandle flatNormal_ = BGFX_INVALID_HANDLE; // 1x1 (0.5, 0.5, 1): stands in for no normal texture
    bgfx::TextureHandle black_ = BGFX_INVALID_HANDLE;      // 1x1 opaque black: a texture2D param's "black" default
    // SCREEN_TEXTURE / DEPTH_TEXTURE (s_kastScreen, s_kastDepth): copies of the target, created on first use.
    bgfx::UniformHandle screenSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle depthSampler_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle target_ = BGFX_INVALID_HANDLE; // setTarget, for one render()
    uint16_t targetWidth_ = 0, targetHeight_ = 0;
    bgfx::TextureHandle screenCopy_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depthCopy_ = BGFX_INVALID_HANDLE;
    uint16_t copyWidth_ = 0, copyHeight_ = 0;
    SceneCopies copies_;

    std::filesystem::path root_;
    MaterialLibrary materials_;
    std::map<std::pair<std::string, TextureUsage>, Texture> textures_; // by (path, usage)
    std::map<std::string, GpuModel> models_;                   // by project-relative path
    std::map<std::string, UserShader> userShaders_;            // by project-relative .kshader path
    std::string previewMaterialPath_, previewTexturePath_;      // setPreview
    Material previewMaterial_;
    bgfx::TextureHandle previewTexture_ = BGFX_INVALID_HANDLE;
    SkyRenderer sky_;
    ParticleRenderer particles_;
    float timeSeconds_ = 0.0f; // setTime
    bool instancing_ = true;
    bool culling_ = true;
};

} // namespace kast
