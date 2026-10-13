#include "assets.h"

#include "build.h"

#include <project.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <format>
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

void scanFolder(const fs::path& dir, const std::string& prefix, const std::optional<fs::path>& hidden,
                std::vector<AssetEntry>& out)
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
        if (entry.folder)
            scanFolder(file, entry.path + '/', hidden, entry.children);
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

std::vector<AssetEntry> scanAssets(const fs::path& root)
{
    std::optional<fs::path> hidden = loadBuildFolder(root);
    if (hidden && (!isSameOrInside(*hidden, root) || isSameOrInside(root, *hidden)))
        hidden.reset(); // outside the project, or the project itself (Build refuses that): hides nothing
    std::vector<AssetEntry> entries;
    scanFolder(root, "", hidden, entries);
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
    return target == source.filename() || moveOnDisk(source, source.parent_path() / target, "rename", error);
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
    return moveOnDisk(source, destDir / source.filename(), "move", error);
}

bool recycleAsset(const fs::path& target, std::string& error)
{
    std::wstring from = fs::absolute(target).lexically_normal().native();
    from.push_back(L'\0'); // a list of names, ended by an empty one
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    // FOF_WANTNUKEWARNING: Windows still asks before deleting for good what it can't recycle (too big, no bin).
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
    const int result = SHFileOperationW(&op);
    if (result == 0 && !op.fAnyOperationsAborted)
        return true;
    error = result != 0 ? std::format("Cannot move '{}' to the Recycle Bin: {} (error {:#x})", utf8(target.filename()),
                                      std::system_category().message(result), result)
                        : std::format("Deleting '{}' was cancelled", utf8(target.filename()));
    return false;
}
