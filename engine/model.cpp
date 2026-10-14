#include "model.h"

#include "animation.h"
#include "files.h"
#include "log.h"
#include "project.h"

#include <assimp/GltfMaterial.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cstring>
#include <format>

namespace kast {
namespace {

constexpr float kDefaultTicksPerSecond = 25.0f; // what Assimp assumes when a file doesn't say

// An in-memory file; Assimp reads every file (the model and its sidecars) through these.
class MemoryStream : public Assimp::IOStream {
public:
    explicit MemoryStream(std::string data) : data_(std::move(data)) {}
    size_t Read(void* buffer, size_t size, size_t count) override
    {
        if (size == 0)
            return 0;
        const size_t items = std::min(count, (data_.size() - pos_) / size);
        std::memcpy(buffer, data_.data() + pos_, items * size);
        pos_ += items * size;
        return items;
    }
    size_t Write(const void*, size_t, size_t) override { return 0; }
    aiReturn Seek(size_t offset, aiOrigin origin) override
    {
        const size_t base = origin == aiOrigin_SET ? 0 : origin == aiOrigin_CUR ? pos_ : data_.size();
        if (base + offset > data_.size())
            return aiReturn_FAILURE;
        pos_ = base + offset;
        return aiReturn_SUCCESS;
    }
    size_t Tell() const override { return pos_; }
    size_t FileSize() const override { return data_.size(); }
    void Flush() override {}

private:
    std::string data_;
    size_t pos_ = 0;
};

std::filesystem::path utf8Path(const char* text)
{
    return std::filesystem::path(reinterpret_cast<const char8_t*>(text));
}

// Reads through the engine's file layer, so models and their sidecar files load from disk or a mounted pak.
class FileLayerIOSystem : public Assimp::IOSystem {
public:
    // ponytail: Exists reads the whole file; Assimp calls it a few times per model load.
    bool Exists(const char* file) const override
    {
        std::string bytes;
        return readFile(utf8Path(file), bytes);
    }
    char getOsSeparator() const override { return '/'; }
    Assimp::IOStream* Open(const char* file, const char* mode) override
    {
        std::string bytes;
        if (std::strchr(mode, 'w') || !readFile(utf8Path(file), bytes))
            return nullptr;
        return new MemoryStream(std::move(bytes));
    }
    void Close(Assimp::IOStream* stream) override { delete stream; }
};

Mat4 toMat4(const aiMatrix4x4& m)
{
    // Assimp: column vectors, row-major storage; bx: row vectors, so the transpose.
    Mat4 out;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            out[col * 4 + row] = m[row][col];
    return out;
}

bx::Vec3 toVec3(const aiVector3D& v) { return {v.x, v.y, v.z}; }
bx::Quaternion toQuat(const aiQuaternion& q) { return {q.x, q.y, q.z, q.w}; } // both Hamilton (see math3d.h)

Trs toTrs(const aiMatrix4x4& m)
{
    aiVector3D scale, position;
    aiQuaternion rotation;
    m.Decompose(scale, rotation, position);
    return {toVec3(position), toQuat(rotation), toVec3(scale)};
}

int metaInt(const aiScene& scene, const char* key, int fallback)
{
    int32_t value = fallback;
    if (scene.mMetaData && scene.mMetaData->Get(key, value))
        return value;
    return fallback;
}

// Meters per file unit. Only FBX says (UnitScaleFactor: centimeters per unit); glTF is meters, OBJ has no unit and is
// taken as meters.
float unitScale(const aiScene& scene)
{
    double factor = 0.0;
    float factorF = 0.0f;
    if (scene.mMetaData && scene.mMetaData->Get("UnitScaleFactor", factor) && factor > 0.0)
        return static_cast<float>(factor * 0.01);
    if (scene.mMetaData && scene.mMetaData->Get("UnitScaleFactor", factorF) && factorF > 0.0f)
        return factorF * 0.01f;
    return 1.0f;
}

// The transform from the file's axes to Y-up. Only FBX says which axes it uses (UpAxis etc. metadata); glTF and OBJ
// are Y-up already. Target X = the file's coord axis, Y = its up axis, Z = its front axis, each with its sign.
aiQuaternion axisFix(const aiScene& scene)
{
    const int up = metaInt(scene, "UpAxis", 1);
    const int front = metaInt(scene, "FrontAxis", 2);
    const int coord = metaInt(scene, "CoordAxis", 0);
    const auto valid = [](int axis) { return axis >= 0 && axis <= 2; };
    if (!valid(up) || !valid(front) || !valid(coord) || up == front || up == coord || front == coord)
        return {};
    aiMatrix3x3 m;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            m[i][j] = 0.0f;
    m[0][coord] = static_cast<float>(metaInt(scene, "CoordAxisSign", 1));
    m[1][up] = static_cast<float>(metaInt(scene, "UpAxisSign", 1));
    m[2][front] = static_cast<float>(metaInt(scene, "FrontAxisSign", 1));
    if (m.Determinant() < 0.0f) // ponytail: a mirrored axis set can't be a rotation; keep the file's axes then
        return {};
    return aiQuaternion(m);
}

struct Loader {
    const aiScene& scene;
    std::string texturePrefix;          // "<model path>" for embedded textures
    std::filesystem::path modelDir;     // project-relative directory of the model, for external textures
    Model model;
    std::unordered_map<std::string, std::vector<int>> nodesByName; // in file order; names can repeat
    std::vector<const aiNode*> sources; // per model node; null for the axis fix node
    std::vector<bool> isJoint;          // per model node: some bone binds to it

    void addNodes(const aiNode& node, int parent)
    {
        const int index = static_cast<int>(model.nodes.size());
        model.nodes.push_back({node.mName.C_Str(), parent, toTrs(node.mTransformation)});
        nodesByName[node.mName.C_Str()].push_back(index);
        sources.push_back(&node);
        for (unsigned i = 0; i < node.mNumChildren; ++i)
            addNodes(*node.mChildren[i], index);
    }

    // How many ancestors nodes a and b share.
    int sharedDepth(int a, int b) const
    {
        const auto path = [this](int n) {
            std::vector<int> nodes;
            for (; n >= 0; n = model.nodes[static_cast<size_t>(n)].parent)
                nodes.insert(nodes.begin(), n);
            return nodes;
        };
        const std::vector<int> pa = path(a), pb = path(b);
        return static_cast<int>(std::ranges::mismatch(pa, pb).in1 - pa.begin());
    }

    // Assimp names a bone's node but nodes can share a name. Of those, the bone binds to the one most tied to the skin:
    // most parent/child nodes named as the mesh's bones, then the deepest ancestor shared with the mesh node, then file
    // order. -1 if no node has the name. (Not aiProcess_PopulateArmatureData's aiBone::mNode: it is a name lookup too,
    // first match in tree order, and a debug Assimp aborts on a file whose root node is a joint.)
    int boneNode(const aiMesh& mesh, const aiBone& bone, int meshNode)
    {
        const auto it = nodesByName.find(bone.mName.C_Str());
        if (it == nodesByName.end())
            return -1;
        if (it->second.size() == 1)
            return it->second.front();
        const auto isBone = [&mesh](const aiNode* node) {
            return node && std::any_of(mesh.mBones, mesh.mBones + mesh.mNumBones,
                                       [node](const aiBone* b) { return b->mName == node->mName; });
        };
        const auto score = [&](int candidate) {
            const aiNode& n = *sources[static_cast<size_t>(candidate)];
            int linked = isBone(n.mParent) ? 1 : 0;
            for (unsigned c = 0; c < n.mNumChildren; ++c)
                linked += isBone(n.mChildren[c]) ? 1 : 0;
            return std::pair{linked, sharedDepth(candidate, meshNode)};
        };
        std::vector<int> best;
        for (int candidate : it->second) {
            if (!best.empty() && score(candidate) < score(best.front()))
                continue;
            if (!best.empty() && score(candidate) > score(best.front()))
                best.clear();
            best.push_back(candidate);
        }
        if (best.size() > 1)
            warnOnce(std::format("Model '{}': {} nodes named '{}' fit its bone equally; binding it to the first",
                                 texturePrefix, best.size(), bone.mName.C_Str()));
        return best.front();
    }

    // A channel only names its node. Of nodes sharing the name, a bone's node wins; if that's still not one node, the
    // first, with a warning (Assimp keeps no more to tell them apart).
    int channelNode(const aiNodeAnim& anim) const
    {
        const auto it = nodesByName.find(anim.mNodeName.C_Str());
        if (it == nodesByName.end())
            return -1;
        if (it->second.size() == 1)
            return it->second.front();
        std::vector<int> joints;
        std::ranges::copy_if(it->second, std::back_inserter(joints),
                             [this](int n) { return isJoint[static_cast<size_t>(n)]; });
        if (joints.size() != 1)
            warnOnce(std::format("Model '{}': {} nodes are named '{}'; clips animate the first{}", texturePrefix,
                                 it->second.size(), anim.mNodeName.C_Str(), joints.empty() ? "" : " joint"));
        return joints.empty() ? it->second.front() : joints.front();
    }

    void addMesh(const aiMesh& source, int node)
    {
        if (!(source.mPrimitiveTypes & aiPrimitiveType_TRIANGLE))
            return;
        ModelMesh mesh;
        mesh.material = source.mMaterialIndex;
        mesh.node = node;
        mesh.vertices.resize(source.mNumVertices);
        for (unsigned v = 0; v < source.mNumVertices; ++v) {
            ModelVertex& out = mesh.vertices[v];
            out = {};
            out.x = source.mVertices[v].x;
            out.y = source.mVertices[v].y;
            out.z = source.mVertices[v].z;
            if (source.mNormals) {
                out.nx = source.mNormals[v].x;
                out.ny = source.mNormals[v].y;
                out.nz = source.mNormals[v].z;
            }
            if (source.mTextureCoords[0]) {
                out.u = source.mTextureCoords[0][v].x;
                out.v = source.mTextureCoords[0][v].y;
            }
        }
        for (unsigned f = 0; f < source.mNumFaces; ++f)
            if (source.mFaces[f].mNumIndices == 3)
                mesh.indices.insert(mesh.indices.end(), source.mFaces[f].mIndices, source.mFaces[f].mIndices + 3);
        if (mesh.indices.empty()) // nothing to draw, and bgfx can't make an empty buffer
            return;
        Mat4 identity;
        bx::mtxIdentity(identity.data());
        for (unsigned b = 0; b < source.mNumBones; ++b) {
            const aiBone& bone = *source.mBones[b];
            const int joint = boneNode(source, bone, node);
            if (joint < 0)
                log::warn(std::format("Model '{}': no node for bone '{}'; its vertices follow the mesh node",
                                      texturePrefix, bone.mName.C_Str()));
            else
                isJoint[static_cast<size_t>(joint)] = true;
            // An unknown bone becomes the mesh node with no inverse bind: it leaves its vertices where they are.
            mesh.joints.push_back(joint < 0 ? node : joint);
            mesh.inverseBinds.push_back(joint < 0 ? identity : toMat4(bone.mOffsetMatrix));
            for (unsigned w = 0; w < bone.mNumWeights; ++w)
                addWeight(mesh.vertices[bone.mWeights[w].mVertexId], static_cast<float>(b), bone.mWeights[w].mWeight);
        }
        int meshNodeJoint = -1; // the same for unweighted vertices, added when the first one needs it
        if (!mesh.joints.empty())
            for (ModelVertex& v : mesh.vertices) {
                float sum = 0.0f;
                for (float weight : v.weights)
                    sum += weight;
                if (sum <= 0.0f) { // all-zero weights would collapse the vertex to the origin
                    if (meshNodeJoint < 0) {
                        meshNodeJoint = static_cast<int>(mesh.joints.size());
                        mesh.joints.push_back(node);
                        mesh.inverseBinds.push_back(identity);
                    }
                    v.joints[0] = static_cast<float>(meshNodeJoint);
                    v.weights[0] = sum = 1.0f;
                }
                for (float& weight : v.weights)
                    weight /= sum;
            }
        model.overBoneLimit = model.overBoneLimit || mesh.joints.size() > kMaxBones;
        model.meshes.push_back(std::move(mesh));
    }

    // Keeps the kMaxBoneWeights largest weights (LimitBoneWeights already did, this guards files it misses).
    static void addWeight(ModelVertex& v, float joint, float weight)
    {
        const auto smallest = std::min_element(std::begin(v.weights), std::end(v.weights));
        if (weight <= *smallest)
            return;
        v.joints[smallest - std::begin(v.weights)] = joint;
        *smallest = weight;
    }

    std::string texture(const aiMaterial& material, aiTextureType type)
    {
        aiString path;
        if (material.GetTexture(type, 0, &path) != AI_SUCCESS)
            return "";
        if (const aiTexture* embedded = scene.GetEmbeddedTexture(path.C_Str())) {
            for (unsigned i = 0; i < scene.mNumTextures; ++i)
                if (scene.mTextures[i] == embedded)
                    return std::format("{}#{}", texturePrefix, i);
            return "";
        }
        std::filesystem::path file = utf8Path(path.C_Str());
        if (file.is_absolute() || file.has_root_name()) // authored on another machine: look next to the model
            file = file.filename();
        const std::u8string relative = (modelDir / file).lexically_normal().generic_u8string();
        return std::string(relative.begin(), relative.end());
    }

    void addMaterials()
    {
        for (unsigned i = 0; i < scene.mNumMaterials; ++i) {
            const aiMaterial& source = *scene.mMaterials[i];
            ModelMaterial slot;
            slot.name = source.GetName().length ? source.GetName().C_Str() : std::format("Material {}", i);
            Material& m = slot.material;
            aiColor4D color;
            if (source.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS) {
                m.baseColor = {color.r, color.g, color.b, color.a};
            } else if (source.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
                float opacity = 1.0f;
                source.Get(AI_MATKEY_OPACITY, opacity);
                m.baseColor = {color.r, color.g, color.b, opacity};
            }
            for (float& channel : m.baseColor)
                channel = bx::clamp(channel, 0.0f, 1.0f);
            source.Get(AI_MATKEY_METALLIC_FACTOR, m.metallic);
            source.Get(AI_MATKEY_ROUGHNESS_FACTOR, m.roughness);
            m.metallic = bx::clamp(m.metallic, 0.0f, 1.0f);
            m.roughness = bx::clamp(m.roughness, 0.0f, 1.0f);
            if (source.Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
                const float peak = bx::max(color.r, bx::max(color.g, color.b));
                if (peak > 0.0f) {
                    // Material colors are 0..1: a brighter emissive moves into the intensity.
                    const float scale = bx::max(peak, 1.0f);
                    float intensity = 1.0f;
                    source.Get(AI_MATKEY_EMISSIVE_INTENSITY, intensity);
                    m.emissiveEnabled = true;
                    m.emissive = {color.r / scale, color.g / scale, color.b / scale};
                    m.emissiveIntensity = bx::max(intensity * scale, 0.0f);
                }
            }
            aiString alphaMode;
            if (source.Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
                const std::string mode = alphaMode.C_Str();
                m.alphaMode = mode == "MASK" ? AlphaMode::Mask : mode == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
                source.Get(AI_MATKEY_GLTF_ALPHACUTOFF, m.alphaCutoff);
                m.alphaCutoff = bx::clamp(m.alphaCutoff, 0.0f, 1.0f);
            } else if (m.baseColor[3] < 1.0f) {
                m.alphaMode = AlphaMode::Blend;
            }
            m.baseColorTexture = texture(source, aiTextureType_BASE_COLOR);
            if (m.baseColorTexture.empty())
                m.baseColorTexture = texture(source, aiTextureType_DIFFUSE);
            m.metallicRoughnessTexture = texture(source, aiTextureType_GLTF_METALLIC_ROUGHNESS);
            model.materials.push_back(std::move(slot));
        }
        // Slots are the materials meshes use, in file order: Assimp adds an unused "DefaultMaterial" to many files.
        std::vector<uint32_t> remap(model.materials.size(), UINT32_MAX);
        for (const ModelMesh& mesh : model.meshes)
            if (mesh.material < remap.size())
                remap[mesh.material] = 0;
        std::vector<ModelMaterial> used;
        for (size_t i = 0; i < model.materials.size(); ++i)
            if (remap[i] == 0) {
                remap[i] = static_cast<uint32_t>(used.size());
                used.push_back(std::move(model.materials[i]));
            }
        model.materials = std::move(used);
        for (ModelMesh& mesh : model.meshes)
            mesh.material = mesh.material < remap.size() ? remap[mesh.material] : 0;
        if (model.materials.empty())
            model.materials.push_back({"Material 0", defaultMaterial()});
    }

    void addTextures()
    {
        for (unsigned i = 0; i < scene.mNumTextures; ++i) {
            const aiTexture& texture = *scene.mTextures[i];
            // ponytail: compressed (PNG/JPG) embedded images only; raw texel arrays (rare) render untextured.
            model.textures.emplace_back(texture.mHeight == 0
                                            ? std::string(reinterpret_cast<const char*>(texture.pcData), texture.mWidth)
                                            : std::string());
        }
    }

    void addClips()
    {
        for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
            const aiAnimation& source = *scene.mAnimations[a];
            const float ticks = source.mTicksPerSecond > 0.0 ? static_cast<float>(source.mTicksPerSecond)
                                                             : kDefaultTicksPerSecond;
            Clip clip;
            clip.name = source.mName.length ? source.mName.C_Str() : std::format("Clip {}", a);
            clip.duration = static_cast<float>(source.mDuration) / ticks;
            for (unsigned c = 0; c < source.mNumChannels; ++c) {
                const aiNodeAnim& anim = *source.mChannels[c];
                const int node = channelNode(anim);
                if (node < 0)
                    continue;
                Channel channel;
                channel.node = node;
                for (unsigned k = 0; k < anim.mNumPositionKeys; ++k)
                    channel.translation.push_back({static_cast<float>(anim.mPositionKeys[k].mTime) / ticks,
                                                   toVec3(anim.mPositionKeys[k].mValue)});
                for (unsigned k = 0; k < anim.mNumRotationKeys; ++k)
                    channel.rotation.push_back({static_cast<float>(anim.mRotationKeys[k].mTime) / ticks,
                                                toQuat(anim.mRotationKeys[k].mValue)});
                for (unsigned k = 0; k < anim.mNumScalingKeys; ++k)
                    channel.scale.push_back({static_cast<float>(anim.mScalingKeys[k].mTime) / ticks,
                                             toVec3(anim.mScalingKeys[k].mValue)});
                clip.channels.push_back(std::move(channel));
            }
            model.clips.push_back(std::move(clip));
        }
    }

    // Bind-pose bounds of every vertex as it renders (skinned through its joints).
    void computeBounds()
    {
        std::vector<Mat4> globals, skin;
        nodeGlobals(model, bindPose(model), globals);
        Aabb box{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
        for (const ModelMesh& mesh : model.meshes) {
            skinMatrices(mesh, globals, skin);
            for (const ModelVertex& v : mesh.vertices) {
                bx::Vec3 p{0.0f, 0.0f, 0.0f};
                if (mesh.joints.empty()) {
                    p = bx::mul(bx::Vec3{v.x, v.y, v.z}, globals[static_cast<size_t>(mesh.node)].data());
                } else {
                    for (size_t i = 0; i < kMaxBoneWeights; ++i)
                        if (v.weights[i] > 0.0f)
                            p = bx::add(p, bx::mul(bx::mul(bx::Vec3{v.x, v.y, v.z},
                                                           skin[static_cast<size_t>(v.joints[i])].data()),
                                                   v.weights[i]));
                }
                box.min = bx::min(box.min, p);
                box.max = bx::max(box.max, p);
            }
        }
        if (box.min.x <= box.max.x)
            model.bounds = box;
    }
};

std::shared_ptr<const Model> convert(const aiScene* scene, Assimp::Importer& importer, std::string prefix,
                                     std::filesystem::path modelDir, std::string& error)
{
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        error = importer.GetErrorString();
        if (error.empty())
            error = "no scene in the file";
        return nullptr;
    }
    Loader loader{*scene, std::move(prefix), std::move(modelDir), {}, {}, {}};
    // Node 0 normalizes the file to meters and Y-up; the file's root follows. A node rather than baked vertices, so
    // skins and clips need no conversion.
    const aiQuaternion fix = axisFix(*scene);
    const float scale = unitScale(*scene);
    const Trs fixTrs{{0.0f, 0.0f, 0.0f}, toQuat(fix), {scale, scale, scale}};
    loader.model.nodes.push_back({"(kast unit and axis fix)", -1, fixTrs});
    loader.sources.push_back(nullptr);
    loader.addNodes(*scene->mRootNode, 0);
    loader.isJoint.resize(loader.sources.size());
    for (size_t index = 1; index < loader.sources.size(); ++index) { // after all nodes: bones look them up by name
        const aiNode& node = *loader.sources[index];
        for (unsigned i = 0; i < node.mNumMeshes; ++i)
            loader.addMesh(*scene->mMeshes[node.mMeshes[i]], static_cast<int>(index));
    }
    if (loader.model.meshes.empty()) {
        error = "no triangle meshes in the file";
        return nullptr;
    }
    loader.addMaterials();
    loader.addTextures();
    loader.addClips();
    loader.computeBounds();
    return std::make_shared<const Model>(std::move(loader.model));
}

constexpr unsigned kImportFlags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
                                  aiProcess_LimitBoneWeights | aiProcess_FlipUVs | aiProcess_SortByPType;

void configure(Assimp::Importer& importer)
{
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, static_cast<int>(kMaxBoneWeights));
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false); // no extra $AssimpFbx$ nodes
    importer.SetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M, false); // unitScale does it (in Assimp 6.0.4 this didn't)
    importer.SetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, false); // keep every joint of a skin
}

} // namespace

const std::vector<std::string>& modelExtensions()
{
    static const std::vector<std::string> extensions{".glb", ".gltf", ".fbx", ".obj"};
    return extensions;
}

bool isModelFile(const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::find(modelExtensions(), ext) != modelExtensions().end();
}

std::string embeddedTextureModel(const std::string& path)
{
    const size_t hash = path.rfind('#');
    if (hash == std::string::npos || !isModelFile(path.substr(0, hash)))
        return "";
    return path.substr(0, hash);
}

std::shared_ptr<const Model> loadModel(const std::filesystem::path& root, const std::string& path, std::string& error)
{
    const std::optional<std::filesystem::path> file = resolveProjectPath(root, path, error);
    if (!file)
        return nullptr;
    if (std::string bytes; !readFile(*file, bytes)) {
        error = "cannot read the file";
        return nullptr;
    }
    Assimp::Importer importer;
    configure(importer);
    importer.SetIOHandler(new FileLayerIOSystem); // the importer owns it
    const std::u8string name = file->generic_u8string();
    const aiScene* scene = importer.ReadFile(reinterpret_cast<const char*>(name.c_str()), kImportFlags);
    return convert(scene, importer, path, std::filesystem::path(path).parent_path(), error);
}

std::shared_ptr<const Model> loadModelFromMemory(std::string_view bytes, const char* hint, std::string& error)
{
    Assimp::Importer importer;
    configure(importer);
    const aiScene* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), kImportFlags, hint);
    return convert(scene, importer, "", "", error);
}

void ModelLibrary::setRoot(const std::filesystem::path& root)
{
    if (root != root_) {
        root_ = root;
        entries_.clear();
    }
}

std::shared_ptr<const Model> ModelLibrary::get(const std::string& path)
{
    if (path.empty() || root_.empty())
        return nullptr;
    auto [it, added] = entries_.try_emplace(path);
    if (added)
        load(path, it->second);
    return it->second.model;
}

void ModelLibrary::reloadChanged()
{
    // ponytail: one stat per cached model per call (per frame), as for materials.
    for (auto& [path, entry] : entries_)
        if (modifiedTime(root_ / path) != entry.mtime)
            load(path, entry);
}

std::optional<std::string_view> ModelLibrary::embeddedTexture(const std::string& reference)
{
    const std::string file = embeddedTextureModel(reference);
    if (file.empty())
        return std::nullopt;
    const std::shared_ptr<const Model> model = get(file);
    size_t index = 0;
    const char* digits = reference.c_str() + file.size() + 1;
    if (!model || std::from_chars(digits, reference.c_str() + reference.size(), index).ec != std::errc{} ||
        index >= model->textures.size() || model->textures[index].empty())
        return std::nullopt;
    return model->textures[index];
}

void ModelLibrary::load(const std::string& path, Entry& entry)
{
    std::string error;
    const std::optional<std::filesystem::path> file = resolveProjectPath(root_, path, error);
    entry.mtime = file ? modifiedTime(*file) : std::nullopt;
    entry.model = loadModel(root_, path, error);
    if (entry.model)
        log::info(std::format("Loaded model '{}': {} meshes, {} nodes, {} clips", path, entry.model->meshes.size(),
                              entry.model->nodes.size(), entry.model->clips.size()));
    else
        warnOnce(std::format("Model '{}': {}; rendering a magenta cube", path, error));
}

ModelLibrary& models()
{
    static ModelLibrary library;
    return library;
}

} // namespace kast
