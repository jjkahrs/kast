#include "model.h"

#include "files.h"
#include "log.h"
#include "project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <type_traits>

namespace kast {
namespace {

static_assert(std::endian::native == std::endian::little, ".kmodel is little-endian, written and read as in memory");
static_assert(sizeof(ModelVertex) == 84 && std::is_trivially_copyable_v<ModelVertex>);
static_assert(sizeof(Key<bx::Vec3>) == 16 && sizeof(Key<bx::Quaternion>) == 20 && sizeof(Mat4) == 64);

constexpr char kModelMagic[4] = {'K', 'M', 'D', 'L'};

struct ModelWriter {
    std::string out;

    template <typename T>
    void pod(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        out.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    void count(size_t n) { pod(static_cast<uint32_t>(n)); }
    void string(std::string_view text)
    {
        count(text.size());
        out.append(text);
    }
    template <typename T>
    void array(const std::vector<T>& values)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        count(values.size());
        out.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
    }
};

// Reads ModelWriter's output; every read fails (false, and stays failed) past the end.
struct ModelReader {
    std::string_view in;
    bool ok = true;

    bool bytes(void* to, size_t size)
    {
        ok = ok && size <= in.size();
        if (ok) {
            std::memcpy(to, in.data(), size);
            in.remove_prefix(size);
        }
        return ok;
    }
    template <typename T>
    T pod(T value = {})
    {
        bytes(&value, sizeof(T));
        return value;
    }
    // A count of items of size bytes each that can still be in the input (so a corrupt count can't allocate much).
    size_t count(size_t size)
    {
        const size_t n = pod<uint32_t>();
        ok = ok && n <= in.size() / std::max<size_t>(size, 1);
        return ok ? n : 0;
    }
    std::string string()
    {
        std::string text(count(1), '\0');
        bytes(text.data(), text.size());
        return text;
    }
    template <typename T>
    void array(std::vector<T>& values, const T& fill)
    {
        values.assign(count(sizeof(T)), fill);
        bytes(values.data(), values.size() * sizeof(T));
    }
};

void writeTrs(ModelWriter& w, const Trs& trs)
{
    w.pod(trs.translation);
    w.pod(trs.rotation);
    w.pod(trs.scale);
}

Trs readTrs(ModelReader& r)
{
    Trs trs;
    trs.translation = r.pod(trs.translation);
    trs.rotation = r.pod(trs.rotation);
    trs.scale = r.pod(trs.scale);
    return trs;
}

// What the renderer and animation index with, in range: a corrupt file must not crash the player.
std::string modelIndexError(const Model& model)
{
    const auto nodeOk = [&](int node) { return node >= 0 && static_cast<size_t>(node) < model.nodes.size(); };
    for (size_t n = 0; n < model.nodes.size(); ++n)
        if (model.nodes[n].parent >= static_cast<int>(n) || (n > 0 && model.nodes[n].parent < 0))
            return std::format("node {} has a bad parent", n);
    if (model.nodes.empty() || model.meshes.empty() || model.materials.empty())
        return "no nodes, meshes or materials";
    for (const ModelMesh& mesh : model.meshes) {
        if (!nodeOk(mesh.node) || mesh.material >= model.materials.size() ||
            mesh.joints.size() != mesh.inverseBinds.size() || !std::ranges::all_of(mesh.joints, nodeOk))
            return "a mesh refers to a missing node, material or joint";
        if (mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
            !std::ranges::all_of(mesh.indices, [&](uint32_t i) { return i < mesh.vertices.size(); }))
            return "a mesh has bad indices";
        const float joints = static_cast<float>(std::max<size_t>(mesh.joints.size(), 1));
        for (const ModelVertex& v : mesh.vertices)
            for (float joint : v.joints)
                if (!(joint >= 0.0f && joint < joints))
                    return "a vertex refers to a missing joint";
    }
    for (const Clip& clip : model.clips)
        if (!std::ranges::all_of(clip.channels, [&](const Channel& c) { return nodeOk(c.node); }))
            return std::format("clip '{}' animates a missing node", clip.name);
    return "";
}

uint64_t fnv1a(uint64_t hash, std::string_view bytes)
{
    for (const char c : bytes)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return (hash ^ 0xffu) * 1099511628211ull; // separator: ("ab", "c") and ("a", "bc") differ
}

std::filesystem::path u8Path(std::string_view text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

std::string u8String(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// A glTF URI's %XX escapes decoded (glTF URIs are RFC 3986: a space is "%20").
std::string decodeUri(std::string_view uri)
{
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        unsigned value = 0;
        if (uri[i] == '%' && i + 2 < uri.size() &&
            std::from_chars(uri.data() + i + 1, uri.data() + i + 3, value, 16).ptr == uri.data() + i + 3) {
            out += static_cast<char>(value);
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

// The external buffer URIs of glTF JSON.
std::vector<std::string> gltfBufferUris(std::string_view json)
{
    std::vector<std::string> uris;
    // ponytail: no "uri" at all (a .glb's own buffer, embedded images) skips parsing, which takes ~0.1 s for a big
    // animated model's JSON on every editor load; parse only "buffers" with a SAX filter if one with URIs gets slow.
    if (json.find("\"uri\"") == std::string_view::npos)
        return uris;
    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (!doc.is_object() || !doc.contains("buffers") || !doc["buffers"].is_array())
        return uris;
    for (const nlohmann::json& buffer : doc["buffers"])
        if (buffer.is_object() && buffer.contains("uri") && buffer["uri"].is_string() &&
            !buffer["uri"].get<std::string>().starts_with("data:"))
            uris.push_back(decodeUri(buffer["uri"].get<std::string>()));
    return uris;
}

// A .glb's JSON chunk (the first, right after the 12-byte header); empty if there is none.
std::string_view glbJson(std::string_view bytes)
{
    if (bytes.size() < 20 || !bytes.starts_with("glTF"))
        return {};
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + 12, 4);
    if (bytes.substr(16, 4) != "JSON" || length > bytes.size() - 20)
        return {};
    return bytes.substr(20, length);
}

// An .obj's mtllib file names: the rest of each "mtllib" line, as Assimp reads it (one name, spaces and all).
std::vector<std::string> objMaterialLibraries(std::string_view text)
{
    std::vector<std::string> names;
    constexpr std::string_view kWhitespace = " \t\r";
    while (!text.empty()) {
        const size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        line.remove_prefix(std::min(line.find_first_not_of(kWhitespace), line.size()));
        if (!line.starts_with("mtllib") || line.size() < 7 || kWhitespace.find(line[6]) == std::string_view::npos)
            continue;
        line.remove_prefix(7);
        line.remove_prefix(std::min(line.find_first_not_of(kWhitespace), line.size()));
        line = line.substr(0, line.find_last_not_of(kWhitespace) + 1);
        if (!line.empty())
            names.emplace_back(line);
    }
    return names;
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

ModelStats modelStats(const Model& model)
{
    ModelStats stats{.meshes = model.meshes.size(), .nodes = model.nodes.size(), .clips = model.clips.size(),
                     .size = bx::sub(model.bounds.max, model.bounds.min)};
    std::vector<int> bones;
    for (const ModelMesh& mesh : model.meshes) {
        stats.vertices += mesh.vertices.size();
        stats.triangles += mesh.indices.size() / 3;
        bones.insert(bones.end(), mesh.joints.begin(), mesh.joints.end());
    }
    std::ranges::sort(bones);
    stats.bones = static_cast<size_t>(std::ranges::distance(bones.begin(), std::ranges::unique(bones).begin()));
    return stats;
}

std::string embeddedTextureModel(const std::string& path)
{
    const size_t hash = path.rfind('#');
    if (hash == std::string::npos || !isModelFile(path.substr(0, hash)))
        return "";
    return path.substr(0, hash);
}

std::string writeModel(const Model& model)
{
    ModelWriter w;
    w.out.append(kModelMagic, sizeof kModelMagic);
    w.pod(kModelFormatVersion);
    w.pod(static_cast<uint32_t>(model.overBoneLimit ? 1 : 0));
    w.pod(model.bounds.min);
    w.pod(model.bounds.max);
    w.count(model.nodes.size());
    for (const ModelNode& node : model.nodes) {
        w.string(node.name);
        w.pod(static_cast<int32_t>(node.parent));
        writeTrs(w, node.bind);
    }
    w.count(model.meshes.size());
    for (const ModelMesh& mesh : model.meshes) {
        w.pod(mesh.material);
        w.pod(static_cast<int32_t>(mesh.node));
        w.array(mesh.vertices);
        w.array(mesh.indices);
        w.array(mesh.joints);
        for (const Mat4& m : mesh.inverseBinds)
            w.pod(m);
    }
    w.count(model.clips.size());
    for (const Clip& clip : model.clips) {
        w.string(clip.name);
        w.pod(clip.duration);
        w.count(clip.channels.size());
        for (const Channel& channel : clip.channels) {
            w.pod(static_cast<int32_t>(channel.node));
            w.array(channel.translation);
            w.array(channel.rotation);
            w.array(channel.scale);
        }
    }
    w.count(model.materials.size());
    for (const ModelMaterial& slot : model.materials) {
        w.string(slot.name);
        w.string(materialToJson(slot.material).dump());
    }
    w.count(model.textures.size());
    for (const std::string& texture : model.textures)
        w.string(texture);
    return std::move(w.out);
}

std::shared_ptr<const Model> readModel(std::string_view bytes, std::string& error)
{
    ModelReader r{bytes};
    char magic[4] = {};
    r.bytes(magic, sizeof magic);
    if (!r.ok || std::memcmp(magic, kModelMagic, sizeof magic) != 0) {
        error = "not a .kmodel file";
        return nullptr;
    }
    if (const uint32_t version = r.pod<uint32_t>(); version != kModelFormatVersion) {
        error = std::format(".kmodel version {}, expected {}", version, kModelFormatVersion);
        return nullptr;
    }
    auto model = std::make_shared<Model>();
    model->overBoneLimit = (r.pod<uint32_t>() & 1) != 0;
    model->bounds.min = r.pod(model->bounds.min);
    model->bounds.max = r.pod(model->bounds.max);
    model->nodes.resize(r.count(4 + 4 + 40));
    for (ModelNode& node : model->nodes) {
        node.name = r.string();
        node.parent = r.pod<int32_t>();
        node.bind = readTrs(r);
    }
    model->meshes.resize(r.count(8 + 12));
    for (ModelMesh& mesh : model->meshes) {
        mesh.material = r.pod<uint32_t>();
        mesh.node = r.pod<int32_t>();
        r.array(mesh.vertices, ModelVertex{});
        r.array(mesh.indices, 0u);
        r.array(mesh.joints, 0);
        mesh.inverseBinds.resize(mesh.joints.size());
        for (Mat4& m : mesh.inverseBinds)
            m = r.pod<Mat4>();
    }
    const Key<bx::Vec3> vec3Key{0.0f, bx::Vec3(bx::InitZero)};
    const Key<bx::Quaternion> quatKey{0.0f, bx::Quaternion(bx::InitIdentity)};
    model->clips.resize(r.count(4 + 4 + 4));
    for (Clip& clip : model->clips) {
        clip.name = r.string();
        clip.duration = r.pod<float>();
        clip.channels.resize(r.count(4 + 12));
        for (Channel& channel : clip.channels) {
            channel.node = r.pod<int32_t>();
            r.array(channel.translation, vec3Key);
            r.array(channel.rotation, quatKey);
            r.array(channel.scale, vec3Key);
        }
    }
    model->materials.resize(r.count(8));
    for (ModelMaterial& slot : model->materials) {
        slot.name = r.string();
        const nlohmann::ordered_json json = nlohmann::ordered_json::parse(r.string(), nullptr, false);
        if (r.ok && !materialFromJson(json, slot.material, error)) {
            error = std::format("material '{}': {}", slot.name, error);
            return nullptr;
        }
    }
    model->textures.resize(r.count(4));
    for (std::string& texture : model->textures)
        texture = r.string();
    if (!r.ok || !r.in.empty()) {
        error = r.ok ? "bytes past the end of the .kmodel" : "the .kmodel is cut short";
        return nullptr;
    }
    if (error = modelIndexError(*model); !error.empty())
        return nullptr;
    return model;
}

std::vector<std::string> modelSidecars(const std::string& path, std::string_view bytes)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::vector<std::string> names = ext == ".gltf"  ? gltfBufferUris(bytes)
                                           : ext == ".glb" ? gltfBufferUris(glbJson(bytes))
                                           : ext == ".obj" ? objMaterialLibraries(bytes)
                                                           : std::vector<std::string>{};
    std::vector<std::string> sidecars;
    for (const std::string& name : names)
        sidecars.push_back(u8String((u8Path(path).parent_path() / u8Path(name)).lexically_normal()));
    return sidecars;
}

std::string modelCacheKey(const std::string& path, std::string_view bytes,
                          const std::vector<std::optional<std::string>>& sidecars, std::string_view version)
{
    uint64_t hash = fnv1a(14695981039346656037ull, version);
    hash = fnv1a(hash, std::to_string(kModelFormatVersion));
    hash = fnv1a(hash, path);
    hash = fnv1a(hash, bytes);
    for (const std::optional<std::string>& sidecar : sidecars)
        hash = fnv1a(fnv1a(hash, sidecar ? "+" : "-"), sidecar ? *sidecar : "");
    return std::format("{:016x}", hash);
}

bool cookModel(const std::filesystem::path& root, const std::string& path, const ModelImporter& importer,
               std::string_view version, const std::filesystem::path& cacheDir, CookedModel& out, std::string& error)
{
    out = {};
    std::string bytes;
    const std::optional<std::filesystem::path> file = resolveProjectPath(root, path, error);
    if (!file)
        return false;
    if (!readFile(*file, bytes)) {
        error = "cannot read the file";
        return false;
    }
    out.sidecars = modelSidecars(path, bytes);
    std::filesystem::path cached;
    if (!version.empty()) {
        std::vector<std::optional<std::string>> sidecarBytes;
        for (const std::string& sidecar : out.sidecars) {
            std::string ignored, data;
            const std::optional<std::filesystem::path> sidecarFile = resolveProjectPath(root, sidecar, ignored);
            sidecarBytes.push_back(sidecarFile && readFile(*sidecarFile, data) ? std::optional(std::move(data)) : std::nullopt);
        }
        cached = cacheDir / (modelCacheKey(path, bytes, sidecarBytes, version) + ".kmodel");
        std::string hit, ignored;
        if (readFile(cached, hit))
            if (std::shared_ptr<const Model> model = readModel(hit, ignored)) { // else: imported again, rewritten
                out.model = std::move(model);
                out.bytes = std::move(hit);
                out.cached = true;
                return true;
            }
    }
    out.model = importer(root, path, error);
    if (!out.model)
        return false;
    out.bytes = writeModel(*out.model);
    if (!cached.empty()) {
        // ponytail: the cache is never pruned; stale keys stay until the folder is deleted (as the shader cache).
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        writeFileAtomic(cached, out.bytes);
    }
    return true;
}

std::string modelPakPath(const std::string& path) { return path + ".kmodel"; }

void ModelLibrary::setRoot(const std::filesystem::path& root)
{
    if (root != root_) {
        root_ = root;
        entries_.clear();
    }
}

void ModelLibrary::setImporter(ModelImporter importer, std::string version)
{
    importer_ = std::move(importer);
    version_ = std::move(version);
    entries_.clear();
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
    // ponytail: one stat per cached model file and sidecar per call (per frame), as for materials.
    for (auto& [path, entry] : entries_)
        if (std::ranges::any_of(entry.mtimes, [&](const auto& file) { return modifiedTime(root_ / u8Path(file.first)) != file.second; }))
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
    const LoadTimer timer(assetLoadStats().models, assetLoadStats().modelSeconds);
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    entry.model = nullptr;
    std::vector<std::string> files{path};
    std::string how = "(cooked)";
    if (importer_) {
        CookedModel cooked;
        if (cookModel(root_, path, importer_, version_, root_ / ".kast" / "model_cache", cooked, error))
            entry.model = std::move(cooked.model);
        files.insert(files.end(), cooked.sidecars.begin(), cooked.sidecars.end());
        how = cooked.cached ? "(cache hit)"
                            : std::format("(imported in {:.2f} s)",
                                          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    } else if (std::string bytes; !readFile(root_ / u8Path(modelPakPath(path)), bytes)) {
        // Logged once: the entry keeps its (pak: unchanging) modification times, so it isn't loaded again.
        log::error(std::format("Model '{}': game.pak has no cooked model for it ('{}'; the editor's Build cooks every "
                               "model file in the project, and only the editor imports them); rendering a magenta cube",
                               path, modelPakPath(path)));
    } else if (!(entry.model = readModel(bytes, error))) {
        error = std::format("cooked model '{}': {}", modelPakPath(path), error);
    }
    entry.mtimes.clear();
    for (const std::string& file : files)
        entry.mtimes.emplace_back(file, modifiedTime(root_ / u8Path(file)));
    if (entry.model)
        log::info(std::format("Loaded model '{}' {}: {} meshes, {} nodes, {} clips", path, how,
                              entry.model->meshes.size(), entry.model->nodes.size(), entry.model->clips.size()));
    else if (!error.empty())
        warnOnce(std::format("Model '{}': {}; rendering a magenta cube", path, error));
}

ModelLibrary& models()
{
    static ModelLibrary library;
    return library;
}

} // namespace kast
