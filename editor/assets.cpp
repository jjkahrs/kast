#include "assets.h"
#include "asset_ids.h"

#include "build.h"
#include "shader_graph_doc.h"

#include <asset_ref.h>
#include <files.h>
#include <log.h>
#include <prefab.h>
#include <project.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <format>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;

int compareNames(const fs::path& a, const fs::path& b)
{
    const std::wstring& left = a.native();
    const std::wstring& right = b.native();
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()),
                                TRUE);
}

bool sameName(const fs::path& a, const fs::path& b) { return compareNames(a, b) == CSTR_EQUAL; }

// A name in dir, other than except, equals name ignoring case.
bool nameTaken(const fs::path& dir, const fs::path& name, const fs::path& except = {})
{
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (sameName(it->path().filename(), name) && it->path().filename() != except)
            return true;
    return false;
}

fs::path clean(const fs::path& path)
{
    fs::path out = path.lexically_normal();
    return out.has_filename() || !out.has_relative_path() ? out : out.parent_path(); // drop a trailing separator
}

// MoveFileEx without MOVEFILE_REPLACE_EXISTING: fails rather than overwrite.
bool moveOnDisk(const fs::path& source, const fs::path& dest, const char* verb, std::string& error)
{
    if (MoveFileExW(source.c_str(), dest.c_str(), 0))
        return true;
    error = std::format("Cannot {} '{}' to '{}': {}", verb, utf8(source.filename()), utf8(dest),
                        std::system_category().message(static_cast<int>(GetLastError())));
    return false;
}

// moveOnDisk, a file's .meta along: if the sidecar can't follow, the file goes back.
bool moveWithMeta(const fs::path& source, const fs::path& dest, const char* verb, std::string& error)
{
    std::error_code ec;
    const fs::path meta = metaFileOf(source);
    const bool withMeta = !fs::is_directory(source, ec) && fs::is_regular_file(meta, ec);
    if (!moveOnDisk(source, dest, verb, error))
        return false;
    if (!withMeta || moveOnDisk(meta, metaFileOf(dest), verb, error))
        return true;
    MoveFileExW(dest.c_str(), source.c_str(), 0);
    return false;
}

void scanFolder(const fs::path& dir, const std::string& prefix, const std::optional<fs::path>& hidden,
                std::vector<AssetEntry>& out, std::vector<fs::directory_entry>* metas)
{
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& file = it->path();
        std::string name = utf8(file.filename());
        if (name.starts_with('.') || (prefix.empty() && sameName(file.filename(), kast::kProjectFileName)) ||
            (hidden && isSameOrInside(file, *hidden)))
            continue;
        std::error_code typeError;
        // A real directory only: junctions and symlinks are leaves, so a link loop can't recurse forever.
        AssetEntry entry{.name = name, .path = prefix + name,
                         .folder = it->symlink_status(typeError).type() == fs::file_type::directory};
        if (!entry.folder && isMetaFile(file.filename())) {
            if (metas)
                metas->push_back(*it);
            continue;
        }
        if (entry.folder)
            scanFolder(file, entry.path + '/', hidden, entry.children, metas);
        out.push_back(std::move(entry));
    }
    std::ranges::sort(out, [](const AssetEntry& a, const AssetEntry& b) {
        if (a.folder != b.folder)
            return a.folder;
        return compareNames(fromUtf8(a.name), fromUtf8(b.name)) == CSTR_LESS_THAN;
    });
}

} // namespace

std::string utf8(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path fromUtf8(std::string_view text)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

bool isMetaFile(const fs::path& name)
{
    const std::wstring& text = name.native();
    const fs::path ext(kMetaExtension);
    return text.size() >= ext.native().size() &&
           sameName(fs::path(text.substr(text.size() - ext.native().size())), ext);
}

fs::path metaFileOf(const fs::path& file)
{
    fs::path meta = file;
    meta += kMetaExtension;
    return meta;
}

std::vector<AssetEntry> scanAssets(const fs::path& root, std::vector<fs::directory_entry>* metas)
{
    std::optional<fs::path> hidden = loadBuildFolder(root);
    std::error_code ec;
    if (hidden && (!isSameOrInside(*hidden, root) || isSameOrInside(root, *hidden) || !fs::exists(*hidden / kGamePakName, ec)))
        hidden.reset(); // outside the project, the project itself (Build refuses that) or not a build: hides nothing
    std::vector<AssetEntry> entries;
    scanFolder(root, "", hidden, entries, metas);
    return entries;
}

const AssetEntry* findAsset(const std::vector<AssetEntry>& entries, std::string_view path)
{
    for (const AssetEntry& entry : entries) {
        if (entry.path == path)
            return &entry;
        if (entry.folder && path.starts_with(entry.path + '/'))
            return findAsset(entry.children, path);
    }
    return nullptr;
}

std::string assetNameError(std::string_view name)
{
    if (name.empty())
        return "the name is empty";
    if (name.starts_with('.'))
        return "names starting with '.' are hidden from the Assets panel";
    if (isMetaFile(fromUtf8(name)))
        return "names ending in '.meta' are asset id files, hidden from the Assets panel";
    if (std::ranges::any_of(name, isInvalidFileChar))
        return R"(the name contains a character Windows does not allow (< > : " / \ | ? * or a control character))";
    if (name.ends_with(' ') || name.ends_with('.'))
        return "the name can't end with a space or '.'";
    if (isReservedFileName(name))
        return std::format("'{}' is a reserved Windows device name", name.substr(0, name.find('.')));
    return {};
}

bool assetNameTaken(const fs::path& dir, const fs::path& name) { return nameTaken(dir, name); }

std::string uniqueFolderName(const fs::path& dir)
{
    for (int n = 1;; ++n) {
        std::string name = n == 1 ? kNewFolderName : std::format("{} {}", kNewFolderName, n);
        if (!nameTaken(dir, fromUtf8(name)))
            return name;
    }
}

bool isSameOrInside(const fs::path& path, const fs::path& ancestor)
{
    const fs::path file = clean(path);
    const fs::path dir = clean(ancestor);
    auto it = file.begin();
    for (const fs::path& part : dir) {
        if (it == file.end() || !sameName(*it, part))
            return false;
        ++it;
    }
    return true;
}

std::optional<fs::path> remapPath(const fs::path& file, const fs::path& from, const fs::path& to)
{
    if (!isSameOrInside(file, from))
        return std::nullopt;
    const fs::path source = clean(file);
    const fs::path prefix = clean(from);
    auto it = source.begin();
    for (std::ptrdiff_t parts = std::distance(prefix.begin(), prefix.end()); parts > 0; --parts)
        ++it;
    fs::path out = clean(to);
    for (; it != source.end(); ++it)
        out /= *it;
    return out;
}

bool remapProjectPaths(std::vector<std::string>& paths, const std::string& from, const std::string& to)
{
    bool changed = false;
    for (std::string& path : paths)
        if (const std::optional<fs::path> moved = remapPath(fromUtf8(path), fromUtf8(from), fromUtf8(to))) {
            path = utf8(*moved);
            changed = true;
        }
    return changed;
}

std::string deleteBlocker(const fs::path& target, const fs::path& openScene)
{
    if (openScene.empty() || !isSameOrInside(openScene, target))
        return {};
    return std::format("Cannot delete '{}': {} the open scene. Open another scene first.", utf8(target.filename()),
                       isSameOrInside(target, openScene) ? "it is" : "it contains");
}

bool createAssetFolder(const fs::path& dir, const std::string& name, std::string& error)
{
    std::string reason = assetNameError(name);
    if (reason.empty() && nameTaken(dir, fromUtf8(name)))
        reason = std::format("'{}' already exists there", name);
    std::error_code ec;
    if (reason.empty() && !fs::create_directory(dir / fromUtf8(name), ec))
        reason = ec ? ec.message() : "it already exists";
    if (reason.empty())
        return true;
    error = std::format("Cannot create folder '{}': {}", name, reason);
    return false;
}

bool saveMaterial(const fs::path& file, const kast::Material& material, std::string& error)
{
    nlohmann::ordered_json json = kast::materialToJson(material);
    kast::writeAssetRefs(json);
    return writeTextFile(file, json.dump(2) + "\n", error);
}

bool writeNewMaterial(const fs::path& dir, fs::path& file, std::string& error)
{
    std::string name = "new_material.mat";
    for (int n = 2; assetNameTaken(dir, fromUtf8(name)); ++n)
        name = std::format("new_material_{}.mat", n);
    file = dir / fromUtf8(name); // ponytail: free when checked, not locked until written
    return saveMaterial(file, kast::Material{}, error);
}

void pushMaterialStep(UndoStack& history, const fs::path& file, std::string name, const kast::Material& before,
                      const kast::Material& after)
{
    if (kast::materialToJson(before) == kast::materialToJson(after))
        return;
    const auto save = [file](const kast::Material& material) {
        return [file, material] {
            if (std::string error; !saveMaterial(file, material, error))
                kast::log::error(std::format("Cannot save material '{}': {}", utf8(file), error));
        };
    };
    history.push({std::move(name), save(before), save(after)});
}

bool writeNewShader(const fs::path& dir, fs::path& file, std::string& error)
{
    std::string name = "new_shader.kshader";
    for (int n = 2; assetNameTaken(dir, fromUtf8(name)); ++n)
        name = std::format("new_shader_{}.kshader", n);
    file = dir / fromUtf8(name); // ponytail: free when checked, not locked until written
    return writeTextFile(file, R"(// A Kast shader (docs/engine/shaders.html). Use it from a material: pick it as the Inspector's Shader.
// Code is bgfx .sc GLSL; fragment() and vertex() read and write the UPPERCASE built-ins.

// Params: one per line, edited per material in the Inspector (and by kast.setMaterialProperty in Lua).
//   param <float|vec2|vec3|vec4|color|texture2D> <name> [= <default>] [range(<min>, <max>)] [srgb];
param color tint = color(1.0, 1.0, 1.0);
param float pulse_speed = 2.0 range(0, 10);
param texture2D detail = white srgb;

// Runs per pixel. Writing nothing looks exactly like the built-in material.
void fragment()
{
    ALBEDO *= tint.rgb * texture2D(detail, UV).rgb;
    EMISSION += tint.rgb * (0.25 + 0.25 * sin(TIME * pulse_speed));
}

// Runs per vertex, in object space (it also moves the shadows). Uncomment to wobble the mesh.
// void vertex()
// {
//     VERTEX += NORMAL * 0.05 * sin(TIME * pulse_speed + VERTEX.y * 4.0);
// }
)", error);
}

bool writeNewShaderGraph(const fs::path& dir, fs::path& file, std::string& error)
{
    std::string name = "new_shader_graph.kshadergraph";
    for (int n = 2; assetNameTaken(dir, fromUtf8(name)); ++n)
        name = std::format("new_shader_graph_{}.kshadergraph", n);
    file = dir / fromUtf8(name); // ponytail: free when checked, not locked until written
    return writeTextFile(file, kast::shaderGraphText(starterShaderGraph()), error);
}

bool writeTextFile(const fs::path& file, std::string_view text, std::string& error)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec || !kast::writeFileAtomic(file, text)) {
        error = std::format("Cannot write '{}'", utf8(file));
        return false;
    }
    return true;
}

bool renameAsset(const fs::path& source, const std::string& name, std::string& error)
{
    const fs::path target = fromUtf8(name);
    std::string reason = assetNameError(name);
    if (reason.empty() && nameTaken(source.parent_path(), target, source.filename()))
        reason = std::format("'{}' already exists there", name);
    if (!reason.empty()) {
        error = std::format("Cannot rename '{}' to '{}': {}", utf8(source.filename()), name, reason);
        return false;
    }
    return target == source.filename() || moveWithMeta(source, source.parent_path() / target, "rename", error);
}

bool moveAsset(const fs::path& source, const fs::path& destDir, std::string& error)
{
    const std::string name = utf8(source.filename());
    if (isSameOrInside(destDir, source)) {
        error = std::format("Cannot move '{}' into itself or one of its folders", name);
        return false;
    }
    if (isSameOrInside(destDir, source.parent_path()) && isSameOrInside(source.parent_path(), destDir))
        return true; // already there
    if (nameTaken(destDir, source.filename())) {
        error = std::format("Cannot move '{}' to '{}': '{}' already exists there", name, utf8(destDir.filename()), name);
        return false;
    }
    return moveWithMeta(source, destDir / source.filename(), "move", error);
}

bool recycleAsset(const fs::path& target, std::string& error)
{
    std::error_code ec;
    const fs::path meta = metaFileOf(target);
    const bool withMeta = !fs::is_directory(target, ec) && fs::is_regular_file(meta, ec);
    return kast::recycleFile(target, error) && (!withMeta || kast::recycleFile(meta, error));
}

std::string uniquePrefabName(const fs::path& dir, const std::string& entityName)
{
    std::string stem = entityName;
    for (char& c : stem)
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view(R"(<>:"/\|?*)").find(c) != std::string_view::npos)
            c = '_';
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        stem.pop_back();
    if (stem.empty() || !assetNameError(stem + kast::kPrefabExtension).empty())
        stem = "Prefab";
    std::string name = stem + kast::kPrefabExtension;
    for (int n = 2; assetNameTaken(dir, fromUtf8(name)); ++n)
        name = std::format("{}_{}{}", stem, n, kast::kPrefabExtension);
    return name;
}

std::vector<std::string> prefabUsers(const fs::path& root, const std::string& target)
{
    std::vector<std::string> users;
    for (const fs::path& file : kast::listFiles(root)) {
        std::string text;
        if ((file.extension() != ".scene" && file.extension() != kast::kPrefabExtension) || !kast::readFile(file, text))
            continue;
        nlohmann::ordered_json json = nlohmann::ordered_json::parse(text, nullptr, false);
        if (!json.is_object() || !json.contains("entities") || !json["entities"].is_array())
            continue; // files that don't parse are skipped
        editAssetRefs(json["entities"], [](std::string& path, std::string& id) { // refs as paths, by id first
            path = (id.empty() ? std::nullopt : assetIds().pathOf(id)).value_or(path);
            id.clear();
        });
        if (std::ranges::any_of(json["entities"], [&](const nlohmann::ordered_json& entity) {
                return entity.is_object() && entity.contains("prefab") && entity["prefab"].is_string() &&
                       isSameOrInside(fromUtf8(entity["prefab"].get<std::string>()), fromUtf8(target));
            }))
            users.push_back(kast::toProjectPath(root, file).value_or(utf8(file)));
    }
    return users;
}
