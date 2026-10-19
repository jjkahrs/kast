#include "files.h"

#include "pak.h"
#include "project.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <system_error>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace kast {
namespace {

struct Mount {
    Pak pak;
    std::filesystem::path root;
};

std::optional<Mount>& mounted()
{
    static std::optional<Mount> mount;
    return mount;
}

} // namespace

bool readFile(const std::filesystem::path& file, std::string& out)
{
    if (const std::optional<Mount>& mount = mounted()) {
        const std::optional<std::string> path = toProjectPath(mount->root, file);
        const std::optional<std::string_view> bytes = path ? mount->pak.find(*path) : std::nullopt;
        if (bytes)
            out.assign(*bytes);
        return bytes.has_value();
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
        return false;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return !in.bad();
}

bool writeFileAtomic(const std::filesystem::path& file, std::string_view bytes)
{
    return writeFileAtomic(file, [&](std::ostream& out) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return true;
    });
}

bool writeFileAtomic(const std::filesystem::path& file, const std::function<bool(std::ostream&)>& write)
{
    std::filesystem::path temp = file;
    temp += ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    const bool written = write(out);
    out.close();
    std::error_code ec;
    if (written && out)
        std::filesystem::rename(temp, file, ec);
    if (!written || !out || ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> listFiles(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> files;
    if (const std::optional<Mount>& mount = mounted()) {
        const std::optional<std::string> path = toProjectPath(mount->root, dir);
        const bool isRoot = std::filesystem::absolute(dir).lexically_normal() ==
                            std::filesystem::absolute(mount->root).lexically_normal();
        if (path || isRoot)
            for (const std::string& entry : mount->pak.list(path.value_or("")))
                files.push_back((mount->root / entry).lexically_normal());
        return files;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::end(it);
         it.increment(ec))
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    std::ranges::sort(files);
    return files;
}

void mountPak(Pak pak, const std::filesystem::path& root)
{
    mounted() = Mount{std::move(pak), std::filesystem::absolute(root).lexically_normal()};
}

bool recycleFile(const std::filesystem::path& target, std::string& error)
{
    const std::u8string u8name = target.filename().u8string();
    const std::string name(reinterpret_cast<const char*>(u8name.data()), u8name.size());
    std::wstring from = std::filesystem::absolute(target).lexically_normal().native();
    from.push_back(L'\0'); // a list of names, ended by an empty one
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    // FOF_WANTNUKEWARNING: Windows still asks before deleting for good what it can't recycle (too big, no bin).
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
    const int result = SHFileOperationW(&op);
    if (result == 0 && !op.fAnyOperationsAborted)
        return true;
    error = result != 0 ? std::format("Cannot move '{}' to the Recycle Bin: {} (error {:#x})", name,
                                      std::system_category().message(result), result)
                        : std::format("Deleting '{}' was cancelled", name);
    return false;
}

} // namespace kast
