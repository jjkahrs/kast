#pragma once

#include "frustum.h"
#include "ibl.h"
#include "lightmap.h"
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kast {

// ponytail: forward shading with a fixed uniform array per draw; clustered lighting if scenes need more per mesh.
constexpr size_t kMaxLocalLights = 8; // MAX_LOCAL_LIGHTS in fs_mesh.sc: point, spot and area lights per draw
constexpr size_t kMaxShadowedPointLights = 4; // s_pointShadow0..3 in fs_mesh.sc
constexpr size_t kMaxShadowedLocalLights = 4; // spot / area tiles of s_csm (atlas tiles 4..7) in fs_mesh.sc
constexpr int kLtcSize = 16; // LTC_SIZE in fs_mesh.sc: entries per axis of the area light table

// A primitive or terrain vertex (meshLayout_). Tangent: xyz along texture u, w the handedness: the bitangent (image up,
// the normal map's +Y) is cross(normal, tangent) * w.
struct MeshVertex {
    float x, y, z;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v; // (0, 0) is the image's top-left
    uint32_t color = 0xffffffff; // RGBA8 (R in the low byte), linear: COLOR0, white
    float u2 = 0.0f, v2 = 0.0f;  // TEXCOORD1: lightmap UV (lightmapped meshes and terrain; docs/engine/lightmaps.html)
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

enum class LocalLightType { Point, Spot, Area };

// A PointLight, SpotLight or AreaLight in world space, as the renderer uses it.
struct LocalLightSample {
    bx::Vec3 position{0.0f, 0.0f, 0.0f}; // area: the rect's center
    float range = 1.0f;
    bx::Vec3 color{0.0f, 0.0f, 0.0f}; // color * intensity
    bool castShadows = false;
    float bias = 0.0f;
    LocalLightType type = LocalLightType::Point;
    bx::Vec3 direction{0.0f, 0.0f, -1.0f};  // spot, area: unit, the way it shines (the entity's world -Z)
    bx::Vec3 halfWidth{0.5f, 0.0f, 0.0f};   // area: from the center to the rect's +X edge
    bx::Vec3 halfHeight{0.0f, 0.5f, 0.0f};  // area: from the center to its +Y edge
    float cosInner = 1.0f, cosOuter = 0.0f; // spot: cosines of its half-angles
    bool twoSided = false;                  // area
    bool baked = false;                     // bakeMode Full: its light is in the lightmaps (lightShinesOn)
};

// Windowed inverse square (Unreal / Frostbite style): saturate(1 - (d/r)^4)^2 / (d^2 + 1). Largest at the light, falls
// smoothly to exactly 0 at distance >= range. fs_mesh.sc evaluates the same formula per pixel (spot lights too).
float pointLightFalloff(float distance, float range);

// A spot light's cone edge: smoothstep from cosOuter (0, at and outside the outer angle) to cosInner (1, inside the
// inner angle), by the cosine of the angle off its direction. fs_mesh.sc spotCone is the same.
float spotConeFalloff(float cosAngle, float cosInner, float cosOuter);

// True if light can light some of the world-space box: point and area lights by their range sphere, spot lights by
// that and their outer cone.
bool lightReaches(const LocalLightSample& light, const Aabb& bounds);

// Indices of the lights of any type (at most kMaxLocalLights) that reach the world-space box, nearest to its center
// first; ties keep list order, the rest are dropped.
std::vector<size_t> nearestLocalLights(const std::vector<LocalLightSample>& lights, const Aabb& bounds);

// Indices of the castShadows lights (at most 4) nearest eye, nearest first; ties keep list order. cubes: point lights
// (kMaxShadowedPointLights cube maps), else spot and area lights (kMaxShadowedLocalLights atlas tiles).
std::vector<size_t> shadowedLights(const std::vector<LocalLightSample>& lights, const bx::Vec3& eye, bool cubes);

// The area light table fs_mesh.sc shades with (engine/shaders/ltc_table.sh, written by tools/ltc_fit): kLtcSize^2
// entries of 8 floats, row by sqrt(1 - N.V), column by roughness.
std::span<const float> ltcTable();

// The form factor (0..1, as fs_mesh.sc ltcIntegrate with the identity transform) of a polygon from a surface point
// with unit normal n: corners relative to the point, clockwise as seen from the side that lights, clipped to the
// surface's horizon. Seen from behind it is 0, or as from the front if twoSided.
float rectFormFactor(const std::array<bx::Vec3, 4>& corners, const bx::Vec3& normal, bool twoSided);

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
// with their materials (PBR metallic-roughness, see fs_mesh.sc) lit by its ambient, its first DirectionalLight and the
// kMaxLocalLights nearest point, spot and area lights each (nearestLocalLights), fogged by its postProcessing.fog, as
// linear HDR color (for PostProcess). The ambient (ibl.h): a mesh inside a baked ReflectionProbe's box takes it from
// that probe (selectProbe); else, with a sky, from the sky's environment cube (built on the GPU when the sky changes,
// in view before the meshes); else the flat ambient color. With the scene's lightmap bake (lightmap.h,
// docs/engine/lightmaps.html) the diffuse part comes from a Static mesh's lightmap or a Dynamic mesh's probe-grid sample
// instead (selectDiffuseGi), and Full lights skip lightmapped draws. Opaque and mask meshes first, then its sky (sky.h), then blended ones and particle
// emitters (particles.h, simulated on the GPU here) far to near. Blended draws whose .kshader samples SCREEN_TEXTURE or
// DEPTH_TEXTURE see a copy of the target's color or depth as the opaque meshes and the sky left it, made only on frames
// such a draw is in view.
// Shadows: cascaded shadow maps for that DirectionalLight, cube shadow maps for the kMaxShadowedPointLights castShadows
// PointLights nearest the eye and perspective atlas tiles for the kMaxShadowedLocalLights castShadows SpotLights and
// AreaLights nearest it, rendered in kShadowViews views before the scene's (see shadow.h).
// renderOverlays draws the editor's: a ground grid, axis lines, a short frustum preview per Camera, the range of the
// selected PointLight (cone of a SpotLight, rect of an AreaLight), the box of the selected ReflectionProbe, the selected
// entity's collider and its ParticleEmitter's shape.
// Every pass culls its draws to its view frustum (main camera, each cascade, each cube face, each tile), and draws that
// share buffers, material, shadow flags and (main pass) lights, unskinned and not blended, are drawn as one GPU-instanced
// submit (see docs/engine/instancing-and-culling.html).
// The caller owns the views' framebuffers, rects and clears; these set the view transform and mode and submit.
class Renderer {
public:
    // The cascades, then 6 cube faces per shadowed point light, then one per shadowed spot / area light.
    static constexpr bgfx::ViewId kShadowViews =
        kMaxShadowCascades + kMaxShadowedPointLights * 6 + kMaxShadowedLocalLights;

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
    // A ReflectionProbe's bake as the renderer sees it: "Not baked", "Baked", or why its files can't be used (the
    // Inspector shows it).
    std::string probeStatus(const ReflectionProbe& probe);
    // The scene's lightmap bake (lighting.lightmaps.bakedPath) as the renderer sees it: "Not baked", "Baked", "Out of
    // date" (the scene's lightmapSceneHash differs from the bake's), or why its files can't be used (the Lighting panel
    // shows it).
    std::string lightmapStatus(const Scene& scene);
    // The editor's Bake: renders scene (and extra, as render() does) into the six faces of a size x size cube at center,
    // without particles and without probes (one bounce: meshes are lit by the sky's environment), prefilters it and reads
    // it back into out. Uses views firstView .. firstView + kShadowViews and runs bgfx::frame() itself (about ten
    // times), so call it between frames, before anything of the next is submitted. False + error if it can't.
    // ponytail: blocking (the window doesn't redraw meanwhile, ~10 frames a probe); spread the faces over frames on
    // spare views if scenes bake many probes at once.
    bool bakeProbe(const Scene& scene, const Scene* extra, const std::filesystem::path& projectRoot,
                   const bx::Vec3& center, uint16_t size, bgfx::ViewId firstView, BakedProbe& out, std::string& error);

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
        Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world space, for culling, local lights and blend sorting
        const Material* material = nullptr;
        const UserShader* shader = nullptr; // the material's .kshader; null: the built-in mesh shader
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        int bones = -1; // index into the frame's bone sets; -1 = not skinned
        bool posed = false; // animated: bounds are the bind pose's, so the draw is never culled
        bool mirrored = false; // world matrix has a negative determinant: front faces wind the other way
        bool castShadows = true;
        bool receiveShadows = true;
        // GI (lightmap.h): a lightmapped draw's lightmap textures (invalid: none) and its place in them; giDynamic: it
        // samples the probe grid (GiMode::Dynamic, or a Static mesh the bake can't light).
        bgfx::TextureHandle lightmap = BGFX_INVALID_HANDLE, lightmapDirection = BGFX_INVALID_HANDLE;
        std::array<float, 4> lightmapST{1.0f, 1.0f, 0.0f, 0.0f};
        bool giDynamic = false;
    };
    // Where a draw's diffuse ambient comes from (fs_mesh.sc u_gi, u_giSh, s_lightmap): lightmap.h selectDiffuseGi.
    struct GiDraw {
        DiffuseGi source = DiffuseGi::Environment;
        Sh9 sh{}; // ProbeGrid: the grid sampled at the draw
    };
    // A scene's bake (lightmap.h), loaded once per bakedPath and again when its lightmaps.json changes on disk.
    struct LightmapSet {
        std::string path;  // bakedPath it was loaded from
        std::string error; // why it can't be used; "" = loaded
        std::optional<std::filesystem::file_time_type> jsonTime;
        LightmapSidecar sidecar;
        std::vector<bgfx::TextureHandle> irradiance, direction; // per atlas page
        std::vector<LightmapMesh> meshes;
        std::map<std::string, size_t> meshIndex;          // LightmapMesh::key -> meshes
        std::map<EntityId, size_t> entities;              // -> sidecar.entities
        std::map<std::pair<size_t, size_t>, Mesh> buffers; // (mesh, submesh): its unwrapped vertices, made on first use
        std::set<std::pair<size_t, size_t>> broken;        // ones that no longer match their source (warned)
    };
    struct ShadowFrame {
        // World to (atlas u, v, depth) per cascade, then (projective) per spot / area light tile: u_csmMtx.
        float csmMtx[kMaxShadowCascades + kMaxShadowedLocalLights][16] = {};
        float csmSplits[4] = {};                   // far view depth of each cascade (unused: the last)
        float csmBias[kMaxShadowCascades][4] = {}; // x: normal offset meters, y: depth bias
        bool sun = false;                          // the DirectionalLight casts shadows
        std::vector<size_t> pointLights;           // shadowed, by cube index: indices into the frame's lights
        std::vector<size_t> tileLights;            // shadowed spot / area lights, by tile: indices into the frame's lights
    };
    // Where a draw's ambient light comes from (fs_mesh.sc u_env*, s_env).
    struct Environment {
        bgfx::TextureHandle cube = BGFX_INVALID_HANDLE; // invalid: none, the flat ambient
        float maxLod = 0.0f;                            // the roughness 1 level
        Sh9 sh{};                                       // already times specular
        bx::Vec3 specular{1.0f, 1.0f, 1.0f};            // tint: the scene's ambient for the sky, intensity for a probe
        bool box = false;                               // a probe's: box-projected onto center +- halfSize
        bx::Vec3 center{0.0f, 0.0f, 0.0f}, halfSize{0.0f, 0.0f, 0.0f};
    };
    // A baked probe's files, loaded once per (bakedPath, resolution) and again when either file changes on disk.
    struct ProbeFile {
        bgfx::TextureHandle cube = BGFX_INVALID_HANDLE; // invalid: error says why
        Sh9 sh{};
        std::string error;
        std::optional<std::filesystem::file_time_type> ktxTime, jsonTime;
    };
    // What the sky's environment cube was built from.
    struct SkyEnvKey {
        SkyMode mode = SkyMode::Off;
        bool cube = false; // the cubemap's cube (else the gradient)
        uint64_t cubeVersion = 0;
        std::array<float, 9> colors{};
        bool operator==(const SkyEnvKey&) const = default;
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
    // Builds the sky's environment cube (skyEnv_, skySh_) in view when the sky changed since; frees it when it's off.
    void updateSkyEnvironment(const SceneSky& sky, bgfx::ViewId view);
    const ProbeFile& probeFile(const ReflectionProbe& probe); // probe.bakedPath is set
    void clearProbes();
    // The scene's bake (bakedPath set), loading or reloading it first; warns once per path when it can't be used.
    LightmapSet& lightmapSet(const std::string& bakedPath);
    void clearLightmaps();
    // Points a Static draw of entity id, submesh submesh of source (LightmapMesh::source) at its unwrapped buffers and
    // atlas page; false (and nothing changed) if the bake has none for it.
    bool applyLightmap(LightmapSet& set, EntityId id, const std::string& source, size_t submesh, DrawItem& item);
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
                              const std::vector<std::vector<Mat4>>& boneSets, const std::vector<LocalLightSample>& lights,
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
    // True if batch can be one instanced submit, with its world matrices in instances (this frame's buffer), and a
    // lightmapped batch's lightmap scale / offsets after them (vs_mesh_instanced.sc i_data4).
    bool instanced(const std::vector<const DrawItem*>& batch, bgfx::InstanceDataBuffer& instances) const;
    void drawGrid(bgfx::ViewId view);
    void drawCameraFrustums(const Scene& scene, bgfx::ViewId view);
    // The selected light's gizmo: a PointLight's range, a SpotLight's cone, an AreaLight's rect, facing and range.
    void drawLightGizmo(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawProbeBox(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawColliderWireframe(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawParticleShape(const Scene& scene, bgfx::ViewId view, entt::entity selected);

    // GPU copy of a TerrainTile's heights: kTileSamples^2 vertices in tile-local metres; indices are terrainIndices_.
    struct TerrainMesh {
        std::shared_ptr<const TerrainTileData> data; // the key's, kept alive so no other data reuses its address
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle lightmap = BGFX_INVALID_HANDLE, lightmapDirection = BGFX_INVALID_HANDLE; // its baked tile
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
    bgfx::UniformHandle localPosRange_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle localColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle localAxis_ = BGFX_INVALID_HANDLE;
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
    bgfx::UniformHandle materialOrm_ = BGFX_INVALID_HANDLE; // u_materialOrm: what s_metallicRoughness's channels hold
    // Material texture stages 7 and 9 (s_normal, s_emissive in fs_mesh.sc): after the shadow samplers on 2..6. Stage 8 is
    // the environment cube (s_env): occlusion shares s_metallicRoughness (stage 1, glTF's ORM packing).
    bgfx::UniformHandle normalSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle envSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle emissiveSampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle envSh_ = BGFX_INVALID_HANDLE;       // u_envSh[9]
    bgfx::UniformHandle envSpecular_ = BGFX_INVALID_HANDLE; // u_envSpecular
    bgfx::UniformHandle envBox_ = BGFX_INVALID_HANDLE;      // u_envBox[2]
    bgfx::UniformHandle gi_ = BGFX_INVALID_HANDLE;          // u_gi
    bgfx::UniformHandle giSh_ = BGFX_INVALID_HANDLE;        // u_giSh[9]
    bgfx::UniformHandle lightmapST_ = BGFX_INVALID_HANDLE;  // u_lightmapST
    bgfx::UniformHandle lightmapSampler_ = BGFX_INVALID_HANDLE, lightmapDirSampler_ = BGFX_INVALID_HANDLE; // 12, 13
    bgfx::UniformHandle csmMtx_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSplits_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmBias_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadowParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle localShadow_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle csmSampler_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::UniformHandle, kMaxShadowedPointLights> pointShadowSamplers_;
    bgfx::UniformHandle kastParams_ = BGFX_INVALID_HANDLE; // u_kastParams: a .kshader's numeric params
    std::array<bgfx::UniformHandle, kMaxShaderTextures> shaderTextureSamplers_; // s_kastTexture0.., its texture2D params
    // Created on first use: a 2x4 atlas of kCascadeSize tiles (2x2 cascades, then 2x2 spot / area light tiles), and per
    // shadowed point light a cube with a frame buffer per face.
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
    bgfx::TextureHandle blackCube_ = BGFX_INVALID_HANDLE;  // 1x1 black cube: s_env without an environment
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
    EnvironmentFilter envFilter_;
    bgfx::TextureHandle skyEnv_ = BGFX_INVALID_HANDLE; // the sky's prefiltered cube; invalid: sky off (or no compute)
    Sh9 skySh_{};
    std::optional<SkyEnvKey> skyEnvKey_; // what skyEnv_ is of; nullopt: nothing built yet
    // ponytail: per Renderer, as the sky's cube: the Scene and Game views each load a probe's cube; share them if
    // scenes get many large probes.
    std::map<std::pair<std::string, int>, ProbeFile> probeFiles_; // by (bakedPath, resolution)
    // ponytail: one bake per Renderer, as the probes: the Scene and Game views each load it; share it if bakes get big.
    LightmapSet lightmaps_; // the last scene's bake (lightmapSet)
    bool capture_ = false; // bakeProbe: no particles, no probes
    ParticleRenderer particles_;
    float timeSeconds_ = 0.0f; // setTime
    bool instancing_ = true;
    bool culling_ = true;
};

} // namespace kast
