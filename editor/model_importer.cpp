#include "model_importer.h"

#include <animation.h>
#include <files.h>
#include <log.h>
#include <project.h>

#include <assimp/GltfMaterial.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/version.h>

#include <meshoptimizer.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <tuple>

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

    // Tangents from CalcTangentSpace (along texture u), made perpendicular to the normal. The handedness comes from the
    // triangles' own UVs rather than Assimp's bitangents: those point along +v of the flipped (top-left) UVs when
    // Assimp computes them but follow the file when a glTF has TANGENTs. Image up (the normal map's +Y) is -v here.
    // Vertices without a usable tangent (no UVs, degenerate UVs) get any unit vector perpendicular to the normal.
    static void addTangents(const aiMesh& source, std::vector<ModelVertex>& vertices, const std::vector<uint32_t>& indices)
    {
        std::vector<bx::Vec3> down(vertices.size(), {0.0f, 0.0f, 0.0f}); // per vertex: sum of d(position)/dv
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const ModelVertex& a = vertices[indices[i]];
            const ModelVertex& b = vertices[indices[i + 1]];
            const ModelVertex& c = vertices[indices[i + 2]];
            const bx::Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
            const float du1 = b.u - a.u, dv1 = b.v - a.v, du2 = c.u - a.u, dv2 = c.v - a.v;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-12f)
                continue;
            const bx::Vec3 dpdv = bx::mul(bx::sub(bx::mul(e2, du1), bx::mul(e1, du2)), 1.0f / det);
            for (size_t k = 0; k < 3; ++k)
                down[indices[i + k]] = bx::add(down[indices[i + k]], dpdv);
        }
        for (size_t v = 0; v < vertices.size(); ++v) {
            ModelVertex& out = vertices[v];
            const bx::Vec3 n{out.nx, out.ny, out.nz};
            bx::Vec3 t{0.0f, 0.0f, 0.0f};
            if (source.mTangents)
                t = toVec3(source.mTangents[v]);
            t = bx::sub(t, bx::mul(n, bx::dot(n, t)));
            if (!(bx::length(t) > 1e-6f)) { // also NaN: Assimp marks tangents it can't compute with qNaN
                const bx::Vec3 axis = std::fabs(n.x) < 0.9f ? bx::Vec3{1.0f, 0.0f, 0.0f} : bx::Vec3{0.0f, 1.0f, 0.0f};
                t = bx::sub(axis, bx::mul(n, bx::dot(n, axis)));
            }
            t = bx::normalize(t);
            out.tx = t.x;
            out.ty = t.y;
            out.tz = t.z;
            out.tw = bx::dot(bx::cross(n, t), down[v]) > 0.0f ? -1.0f : 1.0f;
        }
    }

    void addMesh(const aiMesh& source, int node)
    {
        if (ModelMesh mesh; makeMesh(source, node, mesh))
            model.meshes.push_back(std::move(mesh));
    }

    // False if source has no triangles.
    bool makeMesh(const aiMesh& source, int node, ModelMesh& mesh)
    {
        if (!(source.mPrimitiveTypes & aiPrimitiveType_TRIANGLE))
            return false;
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
            if (source.mTextureCoords[1]) // glTF TEXCOORD_1: lightmap UVs (lightmap_bake.h)
                mesh.uv2.insert(mesh.uv2.end(), {source.mTextureCoords[1][v].x, source.mTextureCoords[1][v].y});
            if (source.mColors[0]) { // glTF COLOR_0 (linear; RGB files come in with alpha 1)
                const aiColor4D& c = source.mColors[0][v];
                const auto byte = [](float f) { return static_cast<uint32_t>(std::lround(bx::clamp(f, 0.0f, 1.0f) * 255.0f)); };
                out.color = byte(c.r) | byte(c.g) << 8 | byte(c.b) << 16 | byte(c.a) << 24;
            }
        }
        for (unsigned f = 0; f < source.mNumFaces; ++f)
            if (source.mFaces[f].mNumIndices == 3)
                mesh.indices.insert(mesh.indices.end(), source.mFaces[f].mIndices, source.mFaces[f].mIndices + 3);
        if (mesh.indices.empty()) // nothing to draw, and bgfx can't make an empty buffer
            return false;
        addTangents(source, mesh.vertices, mesh.indices);
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
        return true;
    }

    // Authored LODs: a later level's mesh merged into the LOD0 mesh of its group with its material, as that mesh's
    // level lod: its vertices appended (moved into the LOD0 mesh's node space at bind, or its joints remapped onto the
    // LOD0 mesh's), its triangles the level's. False (and a warning) if one is skinned and the other isn't.
    // ponytail: one vertex buffer per mesh, so the renderer and the .kmodel treat authored and generated levels alike;
    // the LOD0 draw binds the extra vertices without using them.
    bool mergeLevel(ModelMesh& target, ModelMesh level, size_t lod, const std::vector<Mat4>& globals)
    {
        if (target.joints.empty() != level.joints.empty()) {
            log::warn(std::format("Model '{}': LOD{} of a mesh is {}skinned and its LOD0 isn't; leaving it out",
                                  texturePrefix, lod, level.joints.empty() ? "not " : ""));
            return false;
        }
        if (level.joints.empty() && level.node != target.node) {
            Mat4 inverse, to;
            bx::mtxInverse(inverse.data(), globals[static_cast<size_t>(target.node)].data());
            bx::mtxMul(to.data(), globals[static_cast<size_t>(level.node)].data(), inverse.data());
            for (ModelVertex& v : level.vertices) {
                const bx::Vec3 p = bx::mul(bx::Vec3{v.x, v.y, v.z}, to.data());
                const bx::Vec3 n = bx::normalize(bx::mulXyz0(bx::Vec3{v.nx, v.ny, v.nz}, to.data()));
                const bx::Vec3 t = bx::normalize(bx::mulXyz0(bx::Vec3{v.tx, v.ty, v.tz}, to.data()));
                std::tie(v.x, v.y, v.z, v.nx, v.ny, v.nz, v.tx, v.ty, v.tz) =
                    std::tuple{p.x, p.y, p.z, n.x, n.y, n.z, t.x, t.y, t.z};
            }
        }
        std::vector<float> joints; // level's joint -> target's
        for (size_t j = 0; j < level.joints.size(); ++j) {
            const auto it = std::ranges::find(target.joints, level.joints[j]);
            joints.push_back(static_cast<float>(it - target.joints.begin()));
            if (it == target.joints.end()) {
                target.joints.push_back(level.joints[j]);
                target.inverseBinds.push_back(level.inverseBinds[j]);
            }
        }
        model.overBoneLimit = model.overBoneLimit || target.joints.size() > kMaxBones;
        for (ModelVertex& v : level.vertices)
            for (float& joint : v.joints)
                joint = joints.empty() ? 0.0f : joints[static_cast<size_t>(joint)];
        const auto base = static_cast<uint32_t>(target.vertices.size());
        if (!target.uv2.empty() && level.uv2.empty()) // no lightmap UVs on the level: zeros
            target.uv2.resize(target.uv2.size() + level.vertices.size() * 2);
        else if (!target.uv2.empty()) // (ones the LOD0 mesh lacks are dropped)
            target.uv2.insert(target.uv2.end(), level.uv2.begin(), level.uv2.end());
        target.vertices.insert(target.vertices.end(), level.vertices.begin(), level.vertices.end());
        target.lods.resize(std::max(target.lods.size(), lod));
        for (uint32_t index : level.indices)
            target.lods[lod - 1].push_back(base + index);
        return true;
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
            m.normalTexture = texture(source, aiTextureType_NORMALS);
            if (!m.normalTexture.empty()) {
                source.Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), m.normalScale);
                if (!std::isfinite(m.normalScale))
                    m.normalScale = 1.0f;
            }
            // glTF occlusion comes in as Assimp's lightmap slot (its strength with it); other formats may say AO.
            for (const aiTextureType type : {aiTextureType_LIGHTMAP, aiTextureType_AMBIENT_OCCLUSION}) {
                m.occlusionTexture = texture(source, type);
                if (m.occlusionTexture.empty())
                    continue;
                // Assimp 6's glTF importer stores the strength as "$tex.file.strength", not under its own
                // AI_MATKEY_GLTF_TEXTURE_STRENGTH ("$tex.strength"): read either.
                if (source.Get(_AI_MATKEY_TEXTURE_BASE ".strength", type, 0, m.occlusionStrength) != AI_SUCCESS)
                    source.Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(type, 0), m.occlusionStrength);
                m.occlusionStrength = bx::clamp(m.occlusionStrength, 0.0f, 1.0f);
                break;
            }
            m.emissiveTexture = texture(source, aiTextureType_EMISSIVE);
            // bool reads: glTF stores these as 1-byte bools (an int read would over-read them).
            bool twoSided = false; // glTF doubleSided (FBX / OBJ exporters rarely say)
            if (source.Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided)
                m.cullMode = CullMode::None;
            // KHR_materials_unlit. Not AI_MATKEY_SHADING_MODEL == aiShadingMode_Unlit: OBJ's "illum 0" maps there too.
            // ponytail: Assimp keeps this key "for backwards compatibility"; the glTF import check fails if it goes.
            bool unlit = false;
            m.unlit = source.Get("$mat.gltf.unlit", 0, 0, unlit) == AI_SUCCESS && unlit;
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
                                     std::filesystem::path modelDir, const ModelLodSettings& settings, std::string& error)
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
    // Authored LODs: the meshes of "<base>_LOD<n>" nodes (or meshes). LOD0's are the group's model meshes; later
    // levels merge into them (mergeLevel) once every node is in.
    struct Later {
        const aiMesh* mesh;
        int node;
        std::string base;
        size_t lod;
    };
    std::vector<Later> later;
    std::map<std::string, std::vector<size_t>> groups; // base -> its LOD0 model meshes
    for (size_t index = 1; index < loader.sources.size(); ++index) { // after all nodes: bones look them up by name
        const aiNode& node = *loader.sources[index];
        for (unsigned i = 0; i < node.mNumMeshes; ++i) {
            const aiMesh& mesh = *scene->mMeshes[node.mMeshes[i]];
            std::string base;
            int lod = -1;
            if (!authoredLodName(node.mName.C_Str(), base, lod))
                authoredLodName(mesh.mName.C_Str(), base, lod);
            if (lod > 0) {
                later.push_back({&mesh, static_cast<int>(index), base, static_cast<size_t>(lod)});
                continue;
            }
            const size_t before = loader.model.meshes.size();
            loader.addMesh(mesh, static_cast<int>(index));
            if (lod == 0 && loader.model.meshes.size() > before)
                groups[base].push_back(before);
        }
    }
    std::vector<Mat4> globals;
    nodeGlobals(loader.model, bindPose(loader.model), globals);
    for (const Later& level : later) {
        ModelMesh mesh;
        if (!loader.makeMesh(*level.mesh, level.node, mesh))
            continue;
        const auto group = groups.find(level.base);
        if (group == groups.end() || level.lod >= kMaxModelLods) {
            log::warn(std::format("Model '{}': '{}_LOD{}' has no {}_LOD0 (or is past LOD{}); drawing it as an ordinary mesh",
                                  loader.texturePrefix, level.base, level.lod, level.base, kMaxModelLods - 1));
            loader.model.meshes.push_back(std::move(mesh));
            continue;
        }
        const auto target = std::ranges::find_if(group->second, [&](size_t m) { return loader.model.meshes[m].material == mesh.material; });
        if (target == group->second.end())
            log::warn(std::format("Model '{}': '{}_LOD{}' has a material its LOD0 doesn't; leaving that part out",
                                  loader.texturePrefix, level.base, level.lod));
        else
            loader.mergeLevel(loader.model.meshes[*target], std::move(mesh), level.lod, globals);
    }
    if (loader.model.meshes.empty()) {
        error = "no triangle meshes in the file";
        return nullptr;
    }
    // Every mesh of a group gets all the group's levels (a part a level lacks draws nothing there); the rest get
    // generated ones.
    std::vector<bool> authored(loader.model.meshes.size());
    for (const auto& [base, meshes] : groups) {
        size_t levels = 0;
        for (size_t m : meshes)
            levels = std::max(levels, loader.model.meshes[m].lods.size());
        for (size_t m : meshes) {
            loader.model.meshes[m].lods.resize(levels);
            authored[m] = levels > 0;
        }
    }
    loader.model.lod = settings;
    for (size_t m = 0; m < loader.model.meshes.size(); ++m)
        if (settings.autoLod && !authored[m])
            generateLods(loader.model.meshes[m], settings);
    loader.addMaterials();
    loader.addTextures();
    loader.addClips();
    loader.computeBounds();
    return std::make_shared<const Model>(std::move(loader.model));
}

constexpr unsigned kImportFlags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
                                  aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_FlipUVs |
                                  aiProcess_SortByPType;

void configure(Assimp::Importer& importer)
{
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, static_cast<int>(kMaxBoneWeights));
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false); // no extra $AssimpFbx$ nodes
    importer.SetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M, false); // unitScale does it (in Assimp 6.0.4 this didn't)
    importer.SetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, false); // keep every joint of a skin
}

} // namespace

} // namespace kast

std::shared_ptr<const kast::Model> importModel(const std::filesystem::path& root, const std::string& path,
                                              std::string& error)
{
    using namespace kast;
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
    ModelLodSettings settings;
    std::string text, reason;
    if (const auto settingsFile = resolveProjectPath(root, modelImportSettingsPath(path), reason);
        settingsFile && readFile(*settingsFile, text) && !parseModelImportSettings(text, settings, reason))
        log::warn(std::format("Model '{}': {}: {}; importing with the default settings", path,
                              modelImportSettingsPath(path), reason));
    const aiScene* scene = importer.ReadFile(reinterpret_cast<const char*>(name.c_str()), kImportFlags);
    return convert(scene, importer, path, std::filesystem::path(path).parent_path(), settings, error);
}

std::shared_ptr<const kast::Model> importModelFromMemory(std::string_view bytes, const char* hint, std::string& error)
{
    using namespace kast;
    Assimp::Importer importer;
    configure(importer);
    const aiScene* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), kImportFlags, hint);
    return convert(scene, importer, "", "", {}, error);
}

std::string modelImporterVersion()
{
    // ponytail: bump kImporterRevision when the conversion above changes what it makes from the same file.
    constexpr int kImporterRevision = 3; // 3: LODs
    return std::format("assimp {}.{}.{} rev {:x}; flags {:x}; kast importer {}", aiGetVersionMajor(), aiGetVersionMinor(),
                       aiGetVersionPatch(), aiGetVersionRevision(), kast::kImportFlags, kImporterRevision);
}

bool authoredLodName(std::string_view name, std::string& base, int& level)
{
    const size_t at = name.rfind('_');
    if (at == std::string_view::npos || at == 0 || name.size() < at + 5)
        return false;
    const std::string_view tag = name.substr(at + 1, 3), digits = name.substr(at + 4);
    const auto lower = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; };
    int n = -1;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (!std::ranges::equal(tag, std::string_view("lod"), lower) || ec != std::errc{} ||
        end != digits.data() + digits.size() || n < 0)
        return false;
    base = name.substr(0, at);
    level = n;
    return true;
}

namespace {

// Per vertex: meshopt_SimplifyVertex_Lock if it is on an open edge of the mesh (positions welded, so UV and normal
// seams aren't edges), what sloppy LOD levels keep in place.
std::vector<unsigned char> borderLocks(const kast::ModelMesh& mesh)
{
    std::vector<unsigned> welded(mesh.vertices.size());
    const meshopt_Stream positions{&mesh.vertices[0].x, 3 * sizeof(float), sizeof(kast::ModelVertex)};
    meshopt_generateVertexRemapMulti(welded.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.size(), &positions, 1);
    std::unordered_map<uint64_t, int> edges; // welded (low, high) -> triangles on it
    for (size_t t = 0; t < mesh.indices.size(); t += 3)
        for (size_t k = 0; k < 3; ++k) {
            const uint64_t a = welded[mesh.indices[t + k]], b = welded[mesh.indices[t + (k + 1) % 3]];
            ++edges[std::min(a, b) << 32 | std::max(a, b)];
        }
    std::vector<bool> border(mesh.vertices.size());
    for (const auto& [edge, triangles] : edges)
        if (triangles == 1)
            border[edge >> 32] = border[edge & 0xffffffffu] = true;
    std::vector<unsigned char> locks(mesh.vertices.size());
    for (size_t v = 0; v < locks.size(); ++v)
        locks[v] = welded[v] < border.size() && border[welded[v]] ? meshopt_SimplifyVertex_Lock : 0;
    return locks;
}

} // namespace

void generateLods(kast::ModelMesh& mesh, const kast::ModelLodSettings& settings)
{
    using kast::ModelVertex;
    // The attributes from nx to v: normals and UVs weigh in, tangents don't (they follow the normals).
    constexpr float kWeights[] = {0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    static_assert(offsetof(ModelVertex, v) - offsetof(ModelVertex, nx) == (std::size(kWeights) - 1) * sizeof(float));
    constexpr float kShortBy = 1.1f; // a level over its target by more than this goes sloppy
    mesh.lods.clear();
    std::vector<unsigned char> locks; // borderLocks, once a level needs them
    const size_t triangles = mesh.indices.size() / 3;
    for (size_t level = 1; level < settings.ratios.size(); ++level) {
        const size_t target =
            std::max<size_t>(static_cast<size_t>(std::lround(static_cast<double>(triangles) * settings.ratios[level])), 1);
        std::vector<uint32_t> lod(mesh.indices.size());
        // Error unbounded (1 = the mesh's extent): the ratio decides.
        lod.resize(meshopt_simplifyWithAttributes(lod.data(), mesh.indices.data(), mesh.indices.size(), &mesh.vertices[0].x,
                                                  mesh.vertices.size(), sizeof(ModelVertex), &mesh.vertices[0].nx,
                                                  sizeof(ModelVertex), kWeights, std::size(kWeights), nullptr, target * 3,
                                                  1.0f, meshopt_SimplifyLockBorder, nullptr));
        // ponytail: the topology-keeping simplifier stops short where collapses would flip triangles or break seams
        // (the starter character, many small closed parts, stops at ~91%). Like gltfpack -sa, such a level comes from
        // the sloppy (vertex clustering) simplifier instead, borders still locked; it ignores UV seams, so textures
        // can smear at that level. Per-part simplification if that shows up close.
        if (static_cast<float>(lod.size()) > static_cast<float>(target * 3) * kShortBy) {
            if (locks.empty())
                locks = borderLocks(mesh);
            lod.resize(meshopt_simplifySloppy(lod.data(), mesh.indices.data(), mesh.indices.size(), &mesh.vertices[0].x,
                                              mesh.vertices.size(), sizeof(ModelVertex), locks.data(), target * 3, 1.0f,
                                              nullptr));
        }
        if (lod.empty()) // collapsed entirely: keep the previous level
            lod = mesh.lods.empty() ? mesh.indices : mesh.lods.back();
        mesh.lods.push_back(std::move(lod));
    }
}
