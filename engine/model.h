#pragma once

#include "material.h"
#include "math3d.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Models: glTF/GLB, FBX and OBJ files loaded at runtime through Assimp into GPU-free data (meshes, node hierarchy,
// skins, animation clips, embedded materials). Normalized on load to meters and Y-up (see docs/engine/models-animation.html).
namespace kast {

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
// The model of an embedded texture reference "<model path>#<i>" (the last '#', before it a model file); "" if path
// isn't one, so an ordinary texture file may contain '#'.
std::string embeddedTextureModel(const std::string& path);

// Loads <root>/<path> (disk or mounted pak; sidecar .bin/.mtl/image files resolve next to it). External texture paths
// become project-relative, embedded ones "<path>#<i>". nullptr + error if it can't be read or parsed.
std::shared_ptr<const Model> loadModel(const std::filesystem::path& root, const std::string& path, std::string& error);
// The same from bytes; hint is the format's extension without the dot (e.g. "gltf"). Embedded textures are "#<i>".
std::shared_ptr<const Model> loadModelFromMemory(std::string_view bytes, const char* hint, std::string& error);

// Models by project-relative path, each loaded once, shared by every entity using it, and reloaded when its file's
// modification time changes (disk only: under a pak nothing changes). A file that fails to load gives nullptr and one
// warning. One global library: the renderer, the Animator system, Lua and the editor all read the same models.
// ponytail: global like the component registry; the editor works on one project at a time.
class ModelLibrary {
public:
    // Forgets every cached model if root differs from the last call's.
    void setRoot(const std::filesystem::path& root);
    const std::filesystem::path& root() const { return root_; }
    // nullptr if path is empty, there is no root, or it fails to load (warns once per path).
    std::shared_ptr<const Model> get(const std::string& path);
    // Reloads models whose file changed (or appeared / disappeared) since loaded.
    void reloadChanged();
    // The bytes of an embedded texture reference "<model path>#<i>"; nullopt if there is no such texture.
    std::optional<std::string_view> embeddedTexture(const std::string& reference);

private:
    struct Entry {
        std::shared_ptr<const Model> model;
        std::optional<std::filesystem::file_time_type> mtime;
    };
    void load(const std::string& path, Entry& entry);

    std::filesystem::path root_;
    std::unordered_map<std::string, Entry> entries_;
};

ModelLibrary& models();

} // namespace kast
