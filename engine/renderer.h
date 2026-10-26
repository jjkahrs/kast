#pragma once

#include "frustum.h"
#include "ibl.h"
#include "lightmap.h"
#include "lod.h"
#include "material.h"
#include "model.h"
#include "occlusion.h"
#include "particles.h"
#include "scene.h"
#include "shader.h"
#include "shader_clock.h"
#include "shadow.h"
#include "sky.h"
#include "stats_overlay.h"
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
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kast {

class SdfgiRenderer; // sdfgi.h
class VolumetricFog; // volumetric_fog.h

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
// instead (selectDiffuseGi), and Full lights skip lightmapped draws. A mesh inside an enabled, baked VoxelGI volume's box
// (voxel_gi.h, docs/engine/voxelgi.html; relit on the GPU every frame in the last of the shadow views) cone-traces it for
// its specular and, unless lightmapped, its diffuse. With the scene's SDFGI on (sdfgi.h, docs/engine/sdfgi.html; its
// camera-centered cascades built and relit on the GPU every frame in the last shadow view) a mesh with neither and no
// .kshader takes its diffuse, and if rough its specular, from SDFGI's probes (fs_mesh.sc's SDFGI variant). Opaque and mask meshes first, then its sky (sky.h), then blended ones and particle
// emitters (particles.h, simulated on the GPU here) far to near. Blended draws whose .kshader samples SCREEN_TEXTURE or
// DEPTH_TEXTURE see a copy of the target's color or depth as the opaque meshes and the sky left it, made only on frames
// such a draw is in view.
// Shadows: cascaded shadow maps for that DirectionalLight, cube shadow maps for the kMaxShadowedPointLights castShadows
// PointLights nearest the eye and perspective atlas tiles for the kMaxShadowedLocalLights castShadows SpotLights and
// AreaLights nearest it, rendered in kShadowViews views before the scene's (see shadow.h).
// renderOverlays draws the editor's: a ground grid, axis lines, a short frustum preview per Camera, the range of the
// selected PointLight (cone of a SpotLight, rect of an AreaLight), the box of the selected ReflectionProbe or VoxelGI, the selected
// entity's collider, its ParticleEmitter's shape, its FogVolume's box or sphere and its WorldUI's anchor (a cross) and,
// in World mode, quad outline.
// Volumetric fog (volumetric_fog.h; the scene's postProcessing.volumetricFog, with a target whose fogView setTarget
// gives): its froxel grid lit by the sun, local lights and their shadow maps just rendered in fogView, composited over
// the opaque scene in fogView + 1, and sampled by the blended meshes and particles.
// Every pass culls its draws to its view frustum (main camera, each cascade, each cube face, each tile), and draws that
// share buffers, material, shadow flags and (main pass) lights, unskinned and not blended, are drawn as one GPU-instanced
// submit (see docs/engine/instancing-and-culling.html). Model draws are at the LOD their screen height picks (setLod);
// each level has its own index buffer, so levels batch apart. The camera pass also skips draws a Hi-Z test of an
// earlier frame's depth found hidden (setOcclusion; docs/engine/lod-and-occlusion.html).
// The caller owns the views' framebuffers, rects and clears; these set the view transform and mode and submit.
class Renderer {
public:
    // A textured quad in world space (a World UI panel, world_ui.h): unlit, depth-tested, sorted far to near with the
    // blended draws by its center. corners: top-left, top-right, bottom-right, bottom-left; uv: the texture's rect
    // (u0, v0 at the top-left, u1, v1).
    struct WorldQuad {
        std::array<bx::Vec3, 4> corners{bx::Vec3{0.0f, 0.0f, 0.0f}, bx::Vec3{0.0f, 0.0f, 0.0f},
                                        bx::Vec3{0.0f, 0.0f, 0.0f}, bx::Vec3{0.0f, 0.0f, 0.0f}};
        std::array<float, 4> uv{0.0f, 0.0f, 1.0f, 1.0f};
    };

    // The cascades, then 6 cube faces per shadowed point light, then one per shadowed spot / area light, then the VoxelGI
    // view (its volumes' compute passes, after the shadow maps they read; named "VoxelGI" for its GPU time), then the
    // SDFGI view (its cascades' compute passes; "SDFGI").
    static constexpr bgfx::ViewId kShadowViews =
        kMaxShadowCascades + kMaxShadowedPointLights * 6 + kMaxShadowedLocalLights + 2;

    Renderer();
    ~Renderer();
    void init();
    void shutdown();
    // Materials and textures load from projectRoot (empty = none: every material path renders as an error). Files
    // changed on disk since loaded reload first. shadowViews: the first of kShadowViews views, all before view. With a
    // target (setTarget) the blended draws go to its blendView, after the SCREEN_TEXTURE / DEPTH_TEXTURE copies.
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
    // it can copy them for SCREEN_TEXTURE / DEPTH_TEXTURE, and the later view its blended draws go to (into target;
    // PostProcess::blendView()). Set before each render(); without one (the default) blended draws stay in view and
    // those sample black and the far plane. fogView: the first of the 2 views volumetric fog uses, both between view and
    // blendView (PostProcess::volumetricFogView()); UINT16_MAX (the default): no volumetric fog.
    void setTarget(bgfx::FrameBufferHandle target, uint16_t width, uint16_t height, bgfx::ViewId blendView,
                   bgfx::ViewId fogView = UINT16_MAX);
    // The next render() (not a bake) also draws quads from texture (premultiplied alpha, sRGB values: a GUI's); none after.
    void setWorldQuads(bgfx::TextureHandle texture, std::span<const WorldQuad> quads)
    {
        worldQuadTexture_ = texture;
        worldQuads_.assign(quads.begin(), quads.end());
    }
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
    // Mesh LOD (lod.h): each ModelRenderer draws at the level its bounds' screen height in the camera's view picks
    // (selectLod, with hysteresis) by its LODGroup's thresholds, else its model's (ModelLodSettings); the shadow passes
    // draw the same level. setLod(false): always LOD0 (--no-lod). setLodBias: multiplies every screen height (> 0;
    // above 1 keeps detail further away; --lod-bias). setForcedLod: every model at this level (a mesh with fewer levels
    // draws its last, nothing is culled below the thresholds); -1 (the default) picks by screen height (--force-lod).
    void setLod(bool on) { lod_ = on; }
    void setLodBias(float bias) { lodBias_ = bias; }
    void setForcedLod(int level) { forcedLod_ = level; }
    // Terrain draws its chunks at strides by the same switches (lod.h terrainLevel; a forced level n is stride 1 << n,
    // at most 8; the LOD bias scales distance and error).
    // Logs each ModelRenderer's level when first drawn and each change (--log-lod).
    void setLodLogging(bool on) { lodLogging_ = on; }
    // Terrain LOD test hooks: a checkerboard of strides 1 and 8 by chunk, the worst case for cracks
    // (--terrain-lod-checker); no skirts, so cracks show (--no-terrain-skirts; set before the first frame: built into
    // the tile meshes).
    void setTerrainLodChecker(bool on) { terrainLodChecker_ = on; }
    void setTerrainSkirts(bool on) { terrainSkirts_ = on; }
    // Hi-Z occlusion culling of the camera pass (occlusion.h HizOcclusion), on by default: a draw in the frustum is
    // skipped while the newest test (of a depth 5 frames old) found its dilated box hidden. Never skipped: posed
    // draws, draws with no result yet, everything for a while after a cut or a resize, everything without a target
    // (setTarget) or on a GPU without compute / read-back. setOcclusion(false): none skipped (--no-occlusion).
    // setOcclusionUnsupported (before init): behave as on such a GPU (--occlusion-unsupported).
    void setOcclusion(bool on) { occlusion_ = on; }
    void setOcclusionUnsupported(bool on) { hiz_.setForceUnsupported(on); }
    // Debug views: every draw tinted by its level (LOD0 green, 1 yellow, 2 orange, 3 red, 4+ purple; terrain chunks
    // by stride 1, 2, 4, 8 the same; --lod-tint), the boxes of the draws occlusion skipped in renderOverlays
    // (--show-occluded), and a picture of a Hi-Z pyramid mip (-1 off; --show-hiz N), refreshed as tests run.
    void setLodTint(bool on) { lodTint_ = on; }
    void setShowOccluded(bool on) { showOccluded_ = on; }
    void setHizDebugMip(int mip) { hiz_.setDebugMip(mip); }
    bgfx::TextureHandle hizDebugTexture() const { return hiz_.debugTexture(); }
    // The last render()'s camera pass: triangles and draw calls submitted, draws occlusion skipped, draws per level.
    const RenderStats& stats() const { return stats_; }
    // Frames from a depth to the frame its occlusion results were used, as last measured (0: none yet).
    int occlusionLatency() const { return hiz_.latency(); }
    // The level entity of scene drew at in the last render() that drew it: -1 culled below its last threshold;
    // nullopt if that render() didn't draw it (no model, or not rendered by this Renderer). The LODGroup Inspector shows it.
    std::optional<int> lodLevel(const Scene& scene, entt::entity entity) const;
    // Why the scene's cubemap sky last fell back to the procedural one ("" = it didn't): the Lighting panel shows it.
    const std::string& skyError() const { return sky_.error(); }
    // A ReflectionProbe's bake as the renderer sees it: "Not baked", "Baked", or why its files can't be used (the
    // Inspector shows it).
    std::string probeStatus(const ReflectionProbe& probe);
    // The scene's lightmap bake (lighting.lightmaps.bakedPath) as the renderer sees it: "Not baked", "Baked", "Out of
    // date" (the scene's lightmapSceneHash differs from the bake's), or why its files can't be used (the Lighting panel
    // shows it).
    std::string lightmapStatus(const Scene& scene);
    // A VoxelGI volume's bake (its bakedPath) as the renderer sees it: "Not baked", "Baked", "Out of date" (its
    // voxelGiSceneHash differs from the bake's), or why its files can't be used (the Inspector and Lighting panel show it);
    // "" before the first render() (the project isn't known yet).
    std::string voxelGiStatus(const Scene& scene, entt::entity volume);
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
        uint32_t triangles = 0;
    };
    struct ModelMeshBuffers {
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        std::vector<bgfx::IndexBufferHandle> lods; // per ModelMesh::lods; invalid for an empty level (draws nothing)
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
        uint32_t firstIndex = 0, indexCount = UINT32_MAX; // the range of indices drawn (terrain chunks); default all
        int bones = -1; // index into the frame's bone sets; -1 = not skinned
        size_t submesh = 0; // a model draw's mesh index (Model::meshes)
        int lod = 0;        // a model draw's level (indices is that level's: lodIndices), a terrain run's stride level
        uint32_t triangles = 0; // drawn (stats)
        uint64_t id = 0;        // stable across frames, for occlusion results (drawId); 0: always drawn
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
        Sh9 sh{}; // ProbeGrid (or VoxelGi over it, grid set): the grid sampled at the draw
        int volume = -1;   // the VoxelGI volume (this frame's index) its box selected; -1 none
        bool grid = false; // VoxelGi / Sdfgi: what it fades to (box faces, past the cascades) is the probe grid (sh)
        bool sdfgiSpecular = false; // Sdfgi outside reflection probes: rough reflections from SDFGI
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
        // (mesh, submesh, level): a model LOD's triangles over those unwrapped vertices; invalid: LOD0's are drawn instead.
        std::map<std::tuple<size_t, size_t, int>, bgfx::IndexBufferHandle> lodIndices;
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
    static void destroy(ModelMeshBuffers& mesh);
    // Points each draw whose material has a .kshader at its programs; a shader that fails, or material values of the
    // wrong type for it, draw with errorMaterial() instead.
    void resolveShaders(std::vector<DrawItem>& draws);
    // Builds the GPU meshes of the scene's TerrainTiles (one per TerrainTileData: new data, new mesh), marks them used
    // and appends their draws. pruneTerrain then drops the meshes no rendered scene used this frame.
    // Each chunk at the stride (terrainLevel) the camera's view (viewMtx / projMtx) picks; a draw per run of chunks
    // (Morton order) at one stride and on the same side of the camera's frustum; one per tile when every chunk has
    // the same stride (LOD off or forced).
    void addTerrainDraws(const Scene& scene, const float* viewMtx, const float* projMtx, std::vector<DrawItem>& draws);
    void pruneTerrain();
    void clearTerrain();
    // Appends the draws of a ModelRenderer entity (an error cube if its model fails to load) at the LOD its screen
    // height from eye through projMtx picks (none if culled below its last threshold), recording it in lodStates_.
    void addModelDraws(const Scene& scene, entt::entity entity, const ModelRenderer& renderer, const float* world,
                       const bx::Vec3& eye, const float* projMtx, std::vector<DrawItem>& draws,
                       std::vector<std::vector<Mat4>>& boneSets);
    // The level addModelDraws draws entity's model at: -1 culled below its last threshold.
    int pickLod(const Scene& scene, entt::entity entity, const Model& model, const Aabb& bounds, const bx::Vec3& eye,
                const float* projMtx);
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
    // Draws occlusion found hidden: their boxes for renderOverlays (setShowOccluded), counted in stats_.
    void drawOccludedBoxes(bgfx::ViewId view);
    void drawWorldQuad(bgfx::ViewId view, const WorldQuad& quad);
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
    // The selected ReflectionProbe's or VoxelGI volume's box.
    void drawProbeBox(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawColliderWireframe(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawParticleShape(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawFogVolume(const Scene& scene, bgfx::ViewId view, entt::entity selected);
    void drawWorldUIMarker(const Scene& scene, bgfx::ViewId view, const float* viewMtx, entt::entity selected);

    // GPU copy of a TerrainTile's heights: kTileSamples^2 vertices in tile-local metres, then its skirt vertices
    // (lod.h terrainSkirtVertex); indices are terrainIndices_.
    struct TerrainMesh {
        std::shared_ptr<const TerrainTileData> data; // the key's, kept alive so no other data reuses its address
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        std::vector<Aabb> chunkBounds;                                              // per chunk, world, the surface
        float skirtDepth = 0.0f;                                                    // the deepest skirt
        std::array<std::array<float, kTerrainStrides>, kTerrainChunks> chunkErrors{}; // terrainChunkError per level
        bgfx::TextureHandle lightmap = BGFX_INVALID_HANDLE, lightmapDirection = BGFX_INVALID_HANDLE; // its baked tile
        Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // world space
        bool used = false; // by a tile this frame
    };

    std::array<Mesh, 3> meshes_; // indexed by Primitive
    bgfx::VertexLayout meshLayout_;  // MeshVertex: primitives and terrain
    // Per level (stride 1 << level), without and with skirts: every chunk's indices (appendTerrainChunk) in chunk order,
    // 32-bit, shared by tiles.
    std::array<std::array<bgfx::IndexBufferHandle, 2>, kTerrainStrides> terrainIndices_{};
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
    // The same three with fs_mesh.sc's SDFGI variant (KAST_SDFGI), for the draws that take SDFGI.
    std::array<bgfx::ProgramHandle, 3> sdfgiPrograms_{{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}};
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
    bgfx::ViewId targetBlendView_ = 0;
    bgfx::ViewId targetFogView_ = UINT16_MAX;
    bgfx::UniformHandle volFog_ = BGFX_INVALID_HANDLE; // u_volFog: every mesh draw sets it (w 0: none)
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
    // VoxelGI (voxel_gi.h): its compute passes and the volumes' bakes, loaded once per (bakedPath, resolution) and again
    // when either file changes on disk. Defined in renderer.cpp (voxel_gi.h includes this header).
    struct VoxelGiState;
    std::unique_ptr<VoxelGiState> voxelGi_;
    // SDFGI (sdfgi.h): its cascades follow this Renderer's camera (each viewport has its own).
    std::unique_ptr<SdfgiRenderer> sdfgi_;
    std::unique_ptr<VolumetricFog> volumetricFog_; // its history follows this Renderer's camera
    bool capture_ = false; // bakeProbe: no particles, no probes, no VoxelGI, no SDFGI
    ParticleRenderer particles_;
    std::vector<WorldQuad> worldQuads_; // setWorldQuads, for the next render()
    bgfx::TextureHandle worldQuadTexture_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout worldQuadLayout_; // position, texcoord0
    bgfx::ProgramHandle worldQuadProgram_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle worldQuadSampler_ = BGFX_INVALID_HANDLE;
    float timeSeconds_ = 0.0f; // setTime
    bool instancing_ = true;
    bool culling_ = true;
    bool lod_ = true;
    float lodBias_ = 1.0f;
    int forcedLod_ = -1;
    bool lodLogging_ = false;
    bool terrainLodChecker_ = false, terrainSkirts_ = true;
    bool lodTint_ = false;
    std::map<std::pair<const Material*, int>, Material> lodTints_; // (material, level): its tinted copy
    bool occlusion_ = true;
    HizOcclusion hiz_;
    bool showOccluded_ = false;
    std::vector<Aabb> occludedBoxes_; // the last render()'s, with showOccluded_
    RenderStats stats_;
    // Per (scene, entity) drawn: selectLod's pick (its previous next frame), the level drawn (-1 culled) and the
    // render() (lodFrame_) that drew it; kept across frames (no allocations per frame), entries not drawn pruned now
    // and then.
    struct LodState {
        int selected = 0;
        int drawn = 0;
        uint64_t frame = UINT64_MAX; // none yet
    };
    struct LodKeyHash {
        size_t operator()(const std::pair<const Scene*, entt::entity>& key) const
        {
            return std::hash<const void*>()(key.first) ^ (size_t(entt::to_integral(key.second)) * 0x9e3779b97f4a7c15ull);
        }
    };
    std::unordered_map<std::pair<const Scene*, entt::entity>, LodState, LodKeyHash> lodStates_;
    uint64_t lodFrame_ = 0; // render() calls
    size_t lodPicked_ = 0;  // entities picked in this render()
};

} // namespace kast
