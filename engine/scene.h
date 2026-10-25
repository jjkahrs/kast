#pragma once

#include "audio.h"
#include "math3d.h"

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kast {

using EntityId = uint64_t; // persistent across save/load; 0 is never a valid id

struct PersistentId {
    EntityId value = 0;
};

struct Name {
    std::string value;
};

// Local transform, relative to the parent.
struct Transform {
    bx::Vec3 position{0.0f, 0.0f, 0.0f};
    bx::Quaternion rotation{bx::InitIdentity};
    bx::Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Every scene entity has one. Root entities have parent == entt::null and are ordered by Scene::children(null).
struct Hierarchy {
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

enum class Primitive { Cube, Plane, Sphere };

// How a mesh takes part in global illumination (docs/engine/lightmaps.html). Off: as before GI, its diffuse ambient
// from a reflection probe, the sky or the flat ambient. Static: baked into the scene's lightmaps (it bounces light and
// occludes it; it must not move after the bake); skinned models can't be (treated as Dynamic). Dynamic: its diffuse
// ambient from the baked probe grid at its bounds' center.
enum class GiMode { Off, Static, Dynamic };

// How a light takes part in the lightmap bake. Realtime: not baked at all. Indirect: its bounces are baked, its direct
// light and shadows stay realtime. Full: direct light, shadows and bounces baked, so at run time it no longer lights
// lightmapped Static meshes (it still lights Dynamic and Off ones).
enum class LightBakeMode { Realtime, Indirect, Full };

// material: a project-relative .mat file (see material.h); empty = the default grey material.
// castShadows / receiveShadows: into / from the shadow maps of shadow-casting lights (see renderer.h).
// giMode / lightmapScale: see GiMode; lightmapScale (> 0) multiplies the scene's lightmap texels per meter for it.
struct MeshRenderer {
    Primitive primitive = Primitive::Cube;
    std::string material;
    bool castShadows = true;
    bool receiveShadows = true;
    GiMode giMode = GiMode::Off;
    float lightmapScale = 1.0f;
};

// model: a project-relative .glb/.gltf/.fbx/.obj file (see model.h). materials: per-slot .mat overrides by slot index;
// empty (or past the end) = the model's embedded material.
// castShadows / receiveShadows / giMode / lightmapScale: as MeshRenderer.
struct ModelRenderer {
    std::string model;
    std::vector<std::string> materials;
    bool castShadows = true;
    bool receiveShadows = true;
    GiMode giMode = GiMode::Off;
    float lightmapScale = 1.0f;
};

// Overrides the LOD switching of the entity's ModelRenderer (its model's ModelLodSettings thresholds and cullBelowLast):
// thresholds per level, decreasing screen-height fractions 0..1 (1..kMaxModelLods of them, see lod.h selectLod); bias
// (> 0) multiplies the screen height (above 1 keeps detail further away), on top of the Renderer's global bias;
// cullBelow: not drawn below the last threshold.
struct LODGroup {
    std::vector<float> thresholds{0.5f, 0.25f, 0.1f, 0.03f};
    float bias = 1.0f;
    bool cullBelow = false;
};

constexpr float kMaxAnimatorSpeed = 10.0f;

// Plays the entity's ModelRenderer's skeletal animation clips (see animation.h). Without a ModelRenderer it does
// nothing. clip: a clip name of the model (empty: bind pose).
struct Animator {
    std::string clip;
    bool loop = true;
    float speed = 1.0f; // 0..kMaxAnimatorSpeed
    bool playOnStart = true;
};

constexpr int kMaxShadowCascades = 4;

// Shines along the entity's world -Z axis. castShadows: cascaded shadow maps out to maxDistance meters from the eye,
// in cascadeCount (1..kMaxShadowCascades) slices. bias: meters along the light, normalBias: shadow map texels along
// the surface normal; both push the receiver toward the light against shadow acne.
struct DirectionalLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool castShadows = true;
    float bias = 0.05f;       // >= 0
    float normalBias = 1.5f;  // >= 0
    float maxDistance = 100.0f; // > 0
    int cascadeCount = kMaxShadowCascades;
    LightBakeMode bakeMode = LightBakeMode::Indirect;
};

// Lights meshes within range meters of the entity's world position (see pointLightFalloff in renderer.h).
struct PointLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f}; // linear RGB 0..1
    float intensity = 1.0f;
    float range = 10.0f;
    bool castShadows = false; // a cube shadow map; only the kMaxShadowedPointLights nearest the eye get one
    float bias = 0.05f;       // >= 0, meters toward the light against shadow acne
    LightBakeMode bakeMode = LightBakeMode::Indirect;
};

// Shines along the entity's world -Z axis in a cone: full inside innerAngle, fading smoothly to 0 at outerAngle
// (half-angles in degrees, 0 < innerAngle <= outerAngle < 90), with a PointLight's falloff out to range meters (see
// spotConeFalloff in renderer.h). Scale is ignored.
struct SpotLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f}; // linear RGB 0..1
    float intensity = 1.0f;
    float range = 10.0f;
    float innerAngle = 30.0f;
    float outerAngle = 45.0f;
    bool castShadows = false; // a shadow atlas tile; only the kMaxShadowedLocalLights spot / area lights nearest the eye get one
    float bias = 0.05f;       // >= 0, meters toward the light against shadow acne
    LightBakeMode bakeMode = LightBakeMode::Indirect;
};

// A width x height meter rectangle in the entity's XY plane, centered on it, emitting toward its world -Z (both ways if
// twoSided), shaded with Linearly Transformed Cosines (see docs/engine/coordinates-and-scenes.html). color * intensity is
// its radiance: a surface whose view it fills gets what a PointLight gives at 0 m. range: meters from its center it
// reaches, fading out smoothly before. Scale is ignored.
struct AreaLight {
    bx::Vec3 color{1.0f, 1.0f, 1.0f}; // linear RGB 0..1
    float intensity = 5.0f;
    float width = 1.0f;
    float height = 1.0f;
    float range = 10.0f;
    bool twoSided = false;
    bool castShadows = false; // a shadow atlas tile, as SpotLight's
    float bias = 0.05f;       // >= 0, meters toward the light against shadow acne
    LightBakeMode bakeMode = LightBakeMode::Indirect;
};

// A box-projected reflection probe (see ibl.h, docs/engine/ibl-reflection-probes.html): the editor's Bake renders the
// scene around the entity's world position into a cube of resolution texels per side, saved to bakedPath. Meshes whose
// world bounds' center is inside its box (boxSize meters, axis-aligned, centered on the entity; rotation and scale are
// ignored) take their ambient light from it instead of the sky: specular from the cube, box-projected onto the box,
// diffuse from its spherical harmonics, both times intensity. Where boxes overlap, the highest priority wins, then the
// nearest box center.
struct ReflectionProbe {
    bx::Vec3 boxSize{10.0f, 4.0f, 10.0f}; // > 0 on every axis
    int resolution = 128;                 // 64, 128 or 256
    float intensity = 1.0f;               // >= 0
    int priority = 0;
    std::string bakedPath;                // project-relative .ktx written by Bake; empty = not baked
};

// A VoxelGI volume's cost / quality (voxel_gi.h voxelGiPreset): diffuse cones per pixel, step size, bounces.
enum class VoxelGiQuality { Low, Medium, High };

// A VoxelGI volume (docs/engine/voxelgi.html, voxel_gi.h): the editor's Bake voxelizes the Static meshes (and lightmapped
// Terrain tiles) in its box (boxSize meters, axis-aligned, centered on the entity; rotation and scale are ignored, as a
// ReflectionProbe's) into resolution voxels along its longest axis, saved to bakedPath; every frame the GPU lights them
// with the scene's lights (their shadow maps occluding) and bounces that light, and meshes whose world bounds' center
// is inside the box cone-trace it: their diffuse ambient (unless a lightmap gives it, lightmap.h selectDiffuseGi) and
// their glossy specular, both times intensity, fading to the light outside the box over its last few voxels. Where boxes
// overlap, the highest priority wins, then the nearest box center. Off (enabled false, also from Lua), it lights nothing.
struct VoxelGI {
    bx::Vec3 boxSize{10.0f, 4.0f, 10.0f}; // > 0 on every axis
    int resolution = 128;                 // 64, 128 or 256 voxels along the longest axis
    float intensity = 1.0f;               // >= 0
    VoxelGiQuality quality = VoxelGiQuality::Medium;
    int priority = 0;
    bool enabled = true;
    std::string bakedPath;                // project-relative .json written by Bake; empty = not baked
};

enum class FogVolumeShape { Box, Sphere };

// A local region of volumetric fog (volumetric_fog.h, docs/engine/volumetric-fog.html): while the scene's
// postProcessing.volumetricFog is on, adds density (extinction per meter) scattering with its albedo inside a unit box
// or a sphere 1 across in the entity's world transform (its scale sizes it), fading out over the last edgeFalloff of
// the way from its center to its surface, and with heightFalloff > 0 thinning by exp(-heightFalloff * the height above
// its lowest point).
struct FogVolume {
    FogVolumeShape shape = FogVolumeShape::Box;
    float density = 0.1f;               // >= 0, per meter
    bx::Vec3 albedo{0.9f, 0.9f, 0.9f};  // linear RGB 0..1
    float edgeFalloff = 0.25f;          // 0..1
    float heightFalloff = 0.0f;         // >= 0, per meter; optional in files
};

constexpr float kMinLightmapTexelsPerMeter = 0.25f, kMaxLightmapTexelsPerMeter = 64.0f;
constexpr int kMaxLightmapBounces = 8, kMaxLightmapSamples = 4096;
constexpr float kMinProbeSpacing = 0.25f, kMaxProbeSpacing = 16.0f;

// Per-scene lightmap settings (docs/engine/lightmaps.html), saved under "lighting" > "lightmaps" when not all default.
// The bake settings (texelsPerMeter .. probeSpacing) take effect at the next Bake Lightmaps; the runtime ones (enabled,
// intensity, directional) at once, also from Lua (kast.lightmaps.set). The defaults bake lightmap_room.scene in well
// under a minute on a desktop CPU.
struct SceneLightmaps {
    float texelsPerMeter = 8.0f; // 0.25..64: lightmap texels per meter of Static surface (times each mesh's lightmapScale)
    int bounces = 2;             // 0..8: indirect bounces after the first surface
    int samplesPerTexel = 256;   // 1..4096: paths per lightmap texel (and per probe of the grid)
    float probeSpacing = 1.0f;   // 0.25..16 meters between probes of the grid over the Static meshes
    bool enabled = true;         // the bake lights the scene (off: everything renders as if not baked)
    float intensity = 1.0f;      // >= 0: scales the baked diffuse light (lightmaps and probe grid)
    bool directional = true;     // the lightmaps' dominant direction bends baked light by normal maps
    std::string bakedPath;       // project-relative lightmaps.json written by Bake Lightmaps; empty = not baked
    bool operator==(const SceneLightmaps&) const = default;
};

// SDFGI's cost / quality (sdfgi.h sdfgiPreset): rays per probe per frame, march steps, temporal blend.
enum class SdfgiQuality { Low, Medium, High };

constexpr int kMaxSdfgiCascades = 4;
constexpr float kMinSdfgiCellSize = 0.125f, kMaxSdfgiCellSize = 4.0f;
constexpr float kSdfgiYScales[] = {1.0f, 0.5f, 0.25f}; // SceneSdfgi::yScale's choices

// Per-scene SDFGI settings (docs/engine/sdfgi.html, sdfgi.h), saved under "lighting" > "sdfgi" when not all default:
// signed-distance-field cascades and probe grids that follow the camera, built on the GPU from the Static meshes, static
// models and streamed Terrain tiles, lit live (no bake). Meshes without a lightmap or a VoxelGI volume take their diffuse
// ambient (and rough reflections) from it. enabled and energy also change from Lua (kast.sdfgi.set).
struct SceneSdfgi {
    bool enabled = false;
    int cascades = kMaxSdfgiCascades; // 1..4, each twice the size of the one before
    float cellSize = 0.5f;            // kMinSdfgiCellSize..kMaxSdfgiCellSize: meters per SDF cell of the first cascade
    float energy = 1.0f;              // >= 0: scales its diffuse and reflected light
    float bounceFeedback = 0.5f;      // 0..1: how much of the probes' light surfaces bounce again (0: one bounce)
    SdfgiQuality quality = SdfgiQuality::Medium;
    float yScale = 1.0f;              // kSdfgiYScales: the cascades' height relative to their width (flat open worlds)
    bool operator==(const SceneSdfgi&) const = default;
};

// Per-scene light settings. The defaults match the renderer's former fixed 0.2 grey ambient. Sky off, the ambient is
// that flat color; sky on, image-based lighting from the sky (ibl.h) replaces it, scaled and tinted by these.
struct SceneLighting {
    bx::Vec3 ambientColor{1.0f, 1.0f, 1.0f}; // linear RGB 0..1
    float ambientIntensity = 0.2f;           // >= 0
    SceneLightmaps lightmaps;
    SceneSdfgi sdfgi;
};

enum class SkyMode { Off, Procedural, Cubemap };
constexpr const char* kSkyFaceNames[6] = {"px", "nx", "py", "ny", "pz", "nz"}; // SceneSky::faces order

// Per-scene background drawn behind all geometry (see sky.h). Off: the view clears to its background (or fog) color, as
// before skies, and meshes get the flat ambient. Procedural or Cubemap: it also lights the meshes, as their ambient
// (image-based lighting, ibl.h; SceneLighting scales it).
struct SceneSky {
    SkyMode mode = SkyMode::Off;
    bx::Vec3 top{0.10f, 0.30f, 0.75f};     // linear RGB 0..1: looking straight up
    bx::Vec3 horizon{0.55f, 0.70f, 0.85f}; // at eye level
    bx::Vec3 bottom{0.20f, 0.20f, 0.22f};  // straight down
    // Cubemap: project-relative PNG/JPG images, square and all the same size, looking along +X, -X, +Y, -Y, +Z, -Z
    // (kSkyFaceNames). A face that fails to load shows the procedural sky instead.
    std::array<std::string, 6> faces;
    // Cubemap: a project-relative cube .ktx / .dds (sides +X, -X, +Y, -Y, +Z, -Z as bgfx stores them; Kast's +Z view
    // samples the -Z side, see docs/engine/skybox.html). Set, it is used instead of faces. Saved only when set.
    std::string cube;
};

enum class Tonemap { None, Aces }; // None: clamp to 0..1 (the look before post-processing)

// Per-scene post-processing (see post_process.h) and distance fog (fs_mesh.sc). Every effect starts off, so a scene
// without settings renders as it did before. Ranges are what the scene file accepts.
struct ScenePostProcessing {
    struct Tonemapping {
        bool enabled = false;
        float exposure = 0.0f; // EV, -10..10: color * 2^exposure
        Tonemap op = Tonemap::None;
    } tonemapping;
    struct Bloom {
        bool enabled = false;
        float threshold = 1.0f; // >= 0, HDR luminance where the glow starts
        float intensity = 0.5f; // >= 0
        float radius = 0.5f;    // 0..1, how far the glow spreads
    } bloom;
    struct ColorGrading {
        bool enabled = false;
        float contrast = 1.0f;                // 0..2
        float saturation = 1.0f;              // 0..2
        bx::Vec3 colorFilter{1.0f, 1.0f, 1.0f}; // linear RGB 0..1, multiplied in
        float temperature = 0.0f;             // -1 (cool, blue) .. 1 (warm, orange)
    } colorGrading;
    struct Vignette {
        bool enabled = false;
        float intensity = 0.3f;  // 0..1
        float smoothness = 0.5f; // 0.01..1
    } vignette;
    struct Fxaa {
        bool enabled = false;
    } fxaa;
    struct Fog {
        bool enabled = false;
        bx::Vec3 color{0.5f, 0.6f, 0.7f}; // linear RGB 0..1; the view clears to it while enabled
        float density = 0.05f;          // >= 0, per meter
        float start = 0.0f;             // >= 0, meters from the eye before fog begins
    } fog;
    // Screen-space and volumetric effects (docs/engine/post-processing.html). SSAO and SSR read the opaque scene's
    // G-data (PostProcess::begin), composited before the blended draws; depth of field runs after them, before bloom.
    struct Ssao { // ground-truth AO (GTAO): darkens the indirect diffuse light only
        bool enabled = false;
        float radius = 0.5f;    // meters, 0.05..5: how far around a point occluders count
        float intensity = 1.0f; // 0..4
        float power = 1.5f;     // 0.5..4: falloff, ao^power
        int sliceCount = 2;     // 1..4: horizon search directions per pixel (quality)
    } ssao;
    struct Ssr { // screen-space reflections: replace the probe / IBL specular where the ray hits
        bool enabled = false;
        float maxDistance = 50.0f; // meters, 0.5..1000: the ray's length in the view
        float thickness = 0.3f;    // meters, 0.01..5: how deep a surface counts as hit behind the depth buffer
        float maxRoughness = 0.6f; // 0..1: rougher surfaces keep the probe specular
        float intensity = 1.0f;    // 0..1
        int steps = 32;            // 8..128: ray march steps (quality)
    } ssr;
    struct DepthOfField { // physical thin lens, manual focus, 35 mm full-frame sensor
        bool enabled = false;
        float focusDistance = 10.0f; // meters, 0.1..10000
        float fStop = 2.8f;          // 0.7..32
        float focalLength = 50.0f;   // millimeters, 8..400
        float maxCoc = 16.0f;        // pixels, 1..64: the largest blur radius
    } depthOfField;
    struct VolumetricFog { // froxel in-scattering of the scene's lights; adds to the distance fog above
        bool enabled = false;
        float density = 0.02f;                 // >= 0, extinction per meter at baseHeight
        bx::Vec3 albedo{0.9f, 0.9f, 0.9f};     // linear RGB 0..1: scattered / extinguished
        float anisotropy = 0.2f;               // Henyey-Greenstein g, -0.9..0.9 (> 0: forward scattering)
        float heightFalloff = 0.1f;            // >= 0, per meter: density * exp(-falloff * max(y - baseHeight, 0))
        float baseHeight = 0.0f;               // meters, -10000..10000
        float maxDistance = 100.0f;            // meters, 1..1000: the froxel grid's depth
        float temporalBlend = 0.9f;            // 0..0.99: how much of the last frame's scattering stays
        float lightIntensity = 1.0f;           // >= 0: scales the lights' in-scattering
        float ambientIntensity = 1.0f;         // >= 0: scales the ambient's
    } volumetricFog;
};

// Renders the scene from the entity's world position looking down its world -Z (scale is ignored).
struct Camera {
    float verticalFovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

// Spins the entity's local rotation around its local axis while playing (see rotatorSystem).
struct Rotator {
    bx::Vec3 axis{0.0f, 1.0f, 0.0f};
    float degreesPerSecond = 45.0f;
};

// Gives the entity its own finite state machine in Play mode (see state_machines.h): machine is a definition name
// (fsms/*.lua), initialState the state it enters at Play start (empty or unknown: the definition's initial).
struct StateMachine {
    std::string machine;
    std::string initialState;
};

// Shows a game GUI over the Game view in Play mode (see scene_gui.h): path is a project-relative .rml file.
struct UIDocument {
    std::string path;
    bool visible = true;
};

enum class ColliderShape { Box, Sphere, Capsule, Cylinder };

// A physics shape (see physics.h). Without a Rigidbody the entity is a static body. size: box full extents; radius:
// sphere / capsule / cylinder; height: capsule total height including both hemispheres (>= 2 * radius), or cylinder
// height (any), along local Y. center: local offset. World scale applies: box per axis, the others by the largest axis.
// isTrigger: reports triggerEnter / triggerExit and blocks nothing.
struct Collider {
    ColliderShape shape = ColliderShape::Box;
    bx::Vec3 size{1.0f, 1.0f, 1.0f}; // > 0
    float radius = 0.5f;             // > 0
    float height = 2.0f;             // > 0, >= 2 * radius
    bx::Vec3 center{0.0f, 0.0f, 0.0f};
    bool isTrigger = false;
    float friction = 0.5f;    // >= 0
    float restitution = 0.0f; // 0..1
};

enum class MotionType { Static, Dynamic, Kinematic };

// Makes the entity's Collider move: Dynamic is simulated, Kinematic follows its Transform (and velocity), Static never
// moves. Needs a Collider (without one no body is created).
struct Rigidbody {
    MotionType motionType = MotionType::Dynamic;
    float mass = 1.0f;           // > 0 kg
    float gravityScale = 1.0f;   // multiplies the scene gravity
    float linearDamping = 0.05f; // >= 0
    float angularDamping = 0.05f; // >= 0
};

// An upright capsule moved by kast.physics.moveCharacter (Jolt CharacterVirtual). The entity's position is the
// capsule's bottom (feet); physics never changes its rotation. Not combinable with Collider / Rigidbody.
struct CharacterController {
    float radius = 0.4f;           // > 0
    float height = 1.8f;           // total, >= 2 * radius
    float maxSlopeDegrees = 45.0f; // 0..90, steeper slopes block
    float stepHeight = 0.3f;       // >= 0, steps up to this tall are climbed
};

// Plays a .wav / .mp3 clip (see audio.h) in Play mode: at start if playOnStart, or from Lua (kast.audio.playSource).
struct AudioSource {
    std::string clip; // project-relative .wav / .mp3; empty = none
    AudioBus bus = AudioBus::SFX;
    float volume = 1.0f; // 0..1
    bool loop = false;
    bool playOnStart = false;
};

// Online Services replication (see replication.h; does nothing while the project's Online Services are disabled).
constexpr float kMaxMoveRateHz = 30.0f; // half the world server's 60 messages/s, leaving room for chat and pings

// The local player: while in the world its Transform position (and yaw as heading) is sent as Move at most
// sendRateHz times a second, only when it changed; world.entered places it, world.correction moves it to the server's
// position + the moves still in flight: blended over smoothingMs if that is under smoothingThreshold meters away, else
// snapped.
struct NetworkLocalPlayer {
    float sendRateHz = 10.0f;         // 1..kMaxMoveRateHz
    float smoothingThreshold = 2.0f;  // m, >= 0; 0 = always snap
    float smoothingMs = 100.0f;       // 0..1000, 0 = snap
};

// One per scene. templateEntity: the name of the entity cloned (with its subtree) for every remote player the world
// server spawns; it is taken out of the scene for Play, so it never renders, simulates or reaches scripts.
// interpolationMs: how long a clone takes to reach each new server position (about the senders' Move interval).
// maxExtrapolationMs: how long a clone keeps moving along its last velocity once there, waiting for the next one.
struct NetworkReplication {
    std::string templateEntity;
    float interpolationMs = 100.0f; // 0..1000, 0 = snap
    std::string npcTemplates; // an entity whose children are the NPC templates, by name; empty = none
    float maxExtrapolationMs = 250.0f; // 0..1000, 0 = hold at each server position
};

// Added to each clone: the world server's entity id (an unsigned 32-bit id; ids past 2^31 read negative).
struct NetworkEntity {
    int entityId = 0;
};

// Heightmap terrain (docs/engine/terrain.html): in Play, the tiles of map `map` within `radius` tiles of the local player
// (else the camera) stream in as TerrainTile children. Online, the world server's map replaces `map`.
// lightmap: Bake Lightmaps bakes one lightmap per tile of the map (maps/<folder>/lightmaps/, docs/engine/lightmaps.html)
// and the tiles render with them; off, they keep the sky or flat ambient.
struct Terrain {
    int map = 0;    // map.json id, 0..65535
    int radius = 1; // 0..4 tiles around the centre tile
    bool lightmap = false;
};

struct TerrainTileData; // terrain.h: the tile's heights and its map
// One streamed tile (runtime only, made by TerrainStreamer): rendered and collided at its map position; the entity's
// Transform is ignored.
struct TerrainTile {
    int map = 0, tx = 0, tz = 0;
    std::shared_ptr<const TerrainTileData> data; // not saved
};

// Per-scene physics settings.
struct ScenePhysics {
    bx::Vec3 gravity{0.0f, -9.81f, 0.0f}; // m/s^2
};

// Local-space bounds of each built-in primitive (all fit the unit cube centered on the origin).
Aabb primitiveBounds(Primitive primitive);

// Deepest parent chain a scene may have (a root is level 1): the hierarchy walks recurse, so loading, create and
// reparent refuse anything deeper.
constexpr int kMaxHierarchyDepth = 1000;

// An entity hierarchy on an EnTT registry. Entities are created and destroyed only through Scene so the
// Hierarchy links and root list stay consistent; other components can be added directly via registry().
class Scene {
public:
    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    // New entity with Name, Transform, Hierarchy and a fresh PersistentId, appended to parent's children.
    // Returns entt::null and creates nothing if parent is already kMaxHierarchyDepth levels deep.
    entt::entity create(const std::string& name, entt::entity parent = entt::null);
    // Same, with an explicit id (used by loading). The caller guarantees the id is unique and nonzero.
    entt::entity createWithId(EntityId id, const std::string& name, entt::entity parent = entt::null);
    // A fresh id for createWithId (never handed out again), and keeping ids up to id from create / newId (loading).
    EntityId newId() { return nextId_++; }
    void reserveId(EntityId id) { nextId_ = std::max(nextId_, id + 1); }

    bool valid(entt::entity entity) const { return entity != entt::null && registry_.valid(entity); }

    // Entities created so far by every Scene: unchanged means no entity is new since, for caches keyed by entity.
    static uint64_t creations() { return creations_; }

    // Destroys the entity and its whole subtree. No-op if entity isn't valid.
    void destroy(entt::entity entity);

    // Copies the subtree with new ids, inserted right after the original among its siblings. Returns the copy.
    entt::entity duplicate(entt::entity entity);

    // Moves entity under newParent (entt::null = root), appended last. Keeps the world transform.
    // Returns false and changes nothing if either isn't valid, newParent is entity itself or one of its descendants,
    // or the move would put part of entity's subtree deeper than kMaxHierarchyDepth.
    bool reparent(entt::entity entity, entt::entity newParent);

    // Moves entity to position index among its siblings (past the end: last). No-op if entity isn't valid.
    void setSiblingIndex(entt::entity entity, size_t index);
    // entity's position among its siblings.
    size_t siblingIndex(entt::entity entity) const;

    // True if ancestor is entity or one of its ancestors.
    bool isSelfOrAncestor(entt::entity ancestor, entt::entity entity) const;

    entt::entity parent(entt::entity entity) const { return registry_.get<Hierarchy>(entity).parent; }
    // Levels from the top: 1 for a root.
    int depth(entt::entity entity) const;
    // Ordered children; entt::null gives the roots.
    const std::vector<entt::entity>& children(entt::entity entity) const;

    void localMatrix(entt::entity entity, float* out) const;
    void worldMatrix(entt::entity entity, float* out) const;

    // Depth-first, parents before children, in child order.
    template <typename Fn>
    void forEachDepthFirst(Fn&& fn) const
    {
        for (entt::entity root : roots_)
            visit(root, fn);
    }

    // Lua system scripts in run order, relative to the project root (see project.h, lua_systems.h).
    std::vector<std::string> systems;

    // Saved under "lighting" in the scene file.
    SceneLighting lighting;
    // Saved under "sky" in the scene file.
    SceneSky sky;
    // Saved under "postProcessing" in the scene file.
    ScenePostProcessing postProcessing;
    // Saved under "physics" in the scene file.
    ScenePhysics physics;

    // Opaque data owned by the editor (e.g. camera pose), saved under "editor" in the scene file.
    nlohmann::ordered_json editorData = nlohmann::ordered_json::object();

private:
    template <typename Fn>
    void visit(entt::entity entity, Fn& fn) const
    {
        fn(entity);
        for (entt::entity child : registry_.get<Hierarchy>(entity).children)
            visit(child, fn);
    }

    std::vector<entt::entity>& siblings(entt::entity parent);
    entt::entity copySubtree(entt::entity source, entt::entity parent);
    int height(entt::entity entity) const; // levels in entity's subtree: 1 for a leaf

    entt::registry registry_;
    std::vector<entt::entity> roots_;
    EntityId nextId_ = 1;
    inline static std::atomic<uint64_t> creations_ = 0;
};

// The first entity with a C in depth-first order, or entt::null if there is none. Walks the tree only when more than
// one entity has a C.
template <typename C>
entt::entity firstDepthFirst(const Scene& scene)
{
    const auto view = scene.registry().view<const C>();
    if (view.size() <= 1)
        return view.empty() ? entt::null : view.front();
    entt::entity found = entt::null;
    scene.forEachDepthFirst([&](entt::entity entity) {
        if (found == entt::null && view.contains(entity))
            found = entity;
    });
    return found;
}

// The camera the game renders from: the first Camera entity in depth-first order, or entt::null if there is none.
entt::entity activeCamera(const Scene& scene);

// View and projection matrices of a Camera entity for a viewport of the given width / height.
void cameraMatrices(const Scene& scene, entt::entity camera, float aspect, bool homogeneousDepth, float* view, float* proj);

} // namespace kast
