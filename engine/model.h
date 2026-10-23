#pragma once

#include "material.h"
#include "math3d.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Models: glTF/GLB, FBX and OBJ files as GPU-free data (meshes, node hierarchy, skins, animation clips, embedded
// materials), normalized to meters and Y-up (see docs/engine/models-animation.html). The engine reads only the binary
// .kmodel format below; the Assimp importer is the editor's (editor/model_importer.h, injected with
// ModelLibrary::setImporter), so the player links no Assimp and loads the models Build cooked into game.pak.
//
// .kmodel byte layout (all integers and floats little-endian, floats IEEE 754, no padding):
//   0   char[4]  magic "KMDL"
//   4   u32      format version (kModelFormatVersion)
//   8   u32      flags: bit 0 = overBoneLimit
//   12  f32[6]   bounds: min xyz, max xyz
//   36  sections, back to back. A "string" is u32 length L + L bytes (UTF-8 text or raw bytes); a "count" is a u32.
//   nodes:     count N, then per node: string name, i32 parent, f32[3] translation, f32[4] rotation (x y z w),
//              f32[3] scale
//   meshes:    count N, then per mesh: u32 material, i32 node,
//              count V + V vertices of 84 bytes (ModelVertex in field order: f32 x y z, nx ny nz, tx ty tz tw, u v,
//              joints[4], weights[4], then u32 color),
//              count I + u32[I] indices, count J + i32[J] joints + J x f32[16] inverse binds
//   clips:     count N, then per clip: string name, f32 duration, count C, then per channel: i32 node,
//              count + {f32 time, f32[3]} translation keys, count + {f32 time, f32[4]} rotation keys,
//              count + {f32 time, f32[3]} scale keys
//   materials: count N, then per slot: string name, string material JSON (materialToJson, as in a .mat file)
//   textures:  count N, then per embedded image: string bytes (a PNG/JPG file; empty for one that isn't)
//   end of file: bytes past the textures are an error
namespace kast {

constexpr uint32_t kModelFormatVersion = 1;

constexpr size_t kMaxBones = 128; // MAX_BONES in vs_mesh_skinned.sc: joints per skinned mesh
constexpr size_t kMaxBoneWeights = 4;

using Mat4 = std::array<float, 16>; // bx layout (see math3d.h)

struct Trs {
    bx::Vec3 translation{0.0f, 0.0f, 0.0f};
    bx::Quaternion rotation{bx::InitIdentity};
    bx::Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct ModelVertex {
    float x, y, z;
    float nx, ny, nz;
    float tx, ty, tz, tw; // tangent along texture u; bitangent (image up) = cross(normal, tangent) * tw, as MeshVertex
    float u, v;           // (0, 0) is the image's top-left
    // ponytail: joint indices as floats; Uint8 needs uint shader inputs on DX11. 12 bytes per vertex if it matters.
    float joints[kMaxBoneWeights];
    float weights[kMaxBoneWeights]; // sum to 1 on skinned meshes, all 0 otherwise
    // ponytail: 8 bits per linear channel (glTF COLOR_0 is linear), so dark gradients band; Half x4 if that shows.
    uint32_t color = 0xffffffff; // RGBA8 (R in the low byte), linear: glTF COLOR_0, else white
};

struct ModelNode {
    std::string name;
    int parent = -1; // parents come before their children; node 0 is the root
    Trs bind;        // local rest transform
};

// One submesh: a single material slot.
struct ModelMesh {
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t material = 0;          // slot in Model::materials
    int node = 0;                   // unskinned meshes render at this node's transform
    std::vector<int> joints;        // skin: the node of each bone index; empty = not skinned
    std::vector<Mat4> inverseBinds; // per joint: mesh space to the joint's space at bind time
};

template <typename T>
struct Key {
    float time; // seconds
    T value;
};

// Keys of one node; a missing kind keeps the node's bind value.
struct Channel {
    int node = 0;
    std::vector<Key<bx::Vec3>> translation;
    std::vector<Key<bx::Quaternion>> rotation;
    std::vector<Key<bx::Vec3>> scale;
};

struct Clip {
    std::string name;
    float duration = 0.0f; // seconds
    std::vector<Channel> channels;
};

struct ModelMaterial {
    std::string name; // slot name shown in the Inspector
    Material material;
};

struct Model {
    std::vector<ModelMesh> meshes;
    std::vector<ModelNode> nodes;
    std::vector<Clip> clips;
    std::vector<ModelMaterial> materials;
    // Embedded image files (PNG/JPG bytes); materials refer to texture i as "<model path>#<i>".
    std::vector<std::string> textures;
    Aabb bounds{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; // model space, bind pose
    bool overBoneLimit = false; // some mesh has more than kMaxBones joints: it renders in bind pose, unskinned
};

// The extensions models load from: .glb, .gltf, .fbx, .obj.
const std::vector<std::string>& modelExtensions();
bool isModelFile(const std::filesystem::path& file);

// What the editor's Model Preview reads out about a model.
struct ModelStats {
    size_t meshes = 0;
    size_t vertices = 0;
    size_t triangles = 0;
    size_t nodes = 0;
    size_t bones = 0; // distinct joint nodes across the skinned meshes
    size_t clips = 0;
    bx::Vec3 size{0.0f, 0.0f, 0.0f}; // of the bind-pose bounds, meters
};
ModelStats modelStats(const Model& model);
// The model of an embedded texture reference "<model path>#<i>" (the last '#', before it a model file); "" if path
// isn't one, so an ordinary texture file may contain '#'.
std::string embeddedTextureModel(const std::string& path);

// model as .kmodel bytes.
std::string writeModel(const Model& model);
// .kmodel bytes as a model; nullptr + error if they are not a valid .kmodel of kModelFormatVersion.
std::shared_ptr<const Model> readModel(std::string_view bytes, std::string& error);

// Imports <root>/<path> (a model file; disk or mounted pak) into a Model: the editor's Assimp importer
// (editor/model_importer.h). External texture paths become project-relative, embedded ones "<path>#<i>". nullptr +
// error if it can't be read or parsed.
using ModelImporter =
    std::function<std::shared_ptr<const Model>(const std::filesystem::path& root, const std::string& path, std::string& error)>;

// The project-relative files besides path (a model file holding bytes) that importing it reads: a .gltf's or .glb's
// external buffers (a "uri" that isn't a data: URI), an .obj's mtllib files. Not images: the renderer loads those.
std::vector<std::string> modelSidecars(const std::string& path, std::string_view bytes);
// The model cache's key: a hash of version (the importer's), kModelFormatVersion, path (embedded texture references
// and external texture paths depend on it), the file's bytes and each sidecar's (nullopt: missing). 16 hex digits.
std::string modelCacheKey(const std::string& path, std::string_view bytes,
                          const std::vector<std::optional<std::string>>& sidecars, std::string_view version);

// A model imported through the cache.
struct CookedModel {
    std::shared_ptr<const Model> model;
    std::string bytes;                 // model as .kmodel (writeModel)
    std::vector<std::string> sidecars; // modelSidecars: the other files it is imported from
    bool cached = false;               // read from the cache: the importer didn't run
};
// Imports <root>/<path> with importer, through <cacheDir>/<modelCacheKey>.kmodel when version is set: a hit reads that
// file, a miss imports and writes it. False + error (the importer's) on failure.
bool cookModel(const std::filesystem::path& root, const std::string& path, const ModelImporter& importer,
               std::string_view version, const std::filesystem::path& cacheDir, CookedModel& out, std::string& error);
// Where game.pak keeps model file path cooked ("<path>.kmodel"). The editor's Build writes these entries in place of the
// model files and their sidecars; the player, which has no importer, reads them.
std::string modelPakPath(const std::string& path);

// Models by project-relative path, each loaded once, shared by every entity using it, and reloaded when its file's or a
// sidecar's modification time changes (disk only: under a pak nothing changes). A file that fails to load gives nullptr
// and one warning. One global library: the renderer, the Animator system, Lua and the editor all read the same models.
// With an importer (the editor) models import through it; with a cache version too, through the cache in
// <root>/.kast/model_cache (a dot-folder: never shipped), so an unchanged model is not imported again on the next start.
// Without one (the player) each model is read from modelPakPath instead: game.pak's, never imported.
// ponytail: global like the component registry; the editor works on one project at a time.
class ModelLibrary {
public:
    // Forgets every cached model if root differs from the last call's.
    void setRoot(const std::filesystem::path& root);
    const std::filesystem::path& root() const { return root_; }
    // version: the model cache's (modelCacheKey); empty = no cache. Forgets every cached model.
    void setImporter(ModelImporter importer, std::string version = {});
    // nullptr if path is empty, there is no root, or it fails to load (warns once per path).
    std::shared_ptr<const Model> get(const std::string& path);
    // Reloads models whose file or sidecars changed (or appeared / disappeared) since loaded.
    void reloadChanged();
    // The bytes of an embedded texture reference "<model path>#<i>"; nullopt if there is no such texture.
    std::optional<std::string_view> embeddedTexture(const std::string& reference);

private:
    struct Entry {
        std::shared_ptr<const Model> model;
        // The model file's and its sidecars' (project-relative) modification times when loaded.
        std::vector<std::pair<std::string, std::optional<std::filesystem::file_time_type>>> mtimes;
    };
    void load(const std::string& path, Entry& entry);

    std::filesystem::path root_;
    ModelImporter importer_;
    std::string version_;
    std::unordered_map<std::string, Entry> entries_;
};

ModelLibrary& models();

} // namespace kast
