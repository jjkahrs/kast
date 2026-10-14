#include "pak.h"

#include "files.h"

#include <cstring>
#include <format>
#include <fstream>

namespace kast {
namespace {

constexpr char kMagic[4] = {'K', 'P', 'A', 'K'};

// Little-endian like x64, so values are copied as-is.
template <typename T>
void put(std::string& out, T value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// Reads bytes sequentially; every read checks that it stays inside the data.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    template <typename T>
    bool get(T& value)
    {
        if (data_.size() - at_ < sizeof(T))
            return false;
        std::memcpy(&value, data_.data() + at_, sizeof(T));
        at_ += sizeof(T);
        return true;
    }
    bool get(std::string& text, size_t size)
    {
        if (data_.size() - at_ < size)
            return false;
        text.assign(data_.substr(at_, size));
        at_ += size;
        return true;
    }

private:
    std::string_view data_;
    size_t at_ = 0;
};

} // namespace

bool Pak::open(const std::filesystem::path& file, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = std::format("Cannot open '{}'", file.string());
        return false;
    }
    std::string bytes(std::istreambuf_iterator<char>(in), {});
    if (in.bad()) {
        error = std::format("Cannot read '{}'", file.string());
        return false;
    }
    if (!parse(std::move(bytes), error)) {
        error = std::format("Invalid game data '{}': {}", file.string(), error);
        return false;
    }
    return true;
}

bool Pak::parse(std::string bytes, std::string& error)
{
    Reader reader(bytes);
    char magic[4] = {};
    uint32_t version = 0;
    uint32_t count = 0;
    if (!reader.get(magic) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        error = "not a Kast pak (bad magic)";
        return false;
    }
    if (!reader.get(version) || version != kPakVersion) {
        error = std::format("unsupported pak version {} (expected {})", version, kPakVersion);
        return false;
    }
    if (!reader.get(count)) {
        error = "truncated header";
        return false;
    }
    std::map<std::string, Range> index;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length = 0;
        std::string path;
        Range range;
        if (!reader.get(length) || !reader.get(path, length) || !reader.get(range.offset) || !reader.get(range.size)) {
            error = std::format("truncated index (entry {} of {})", i + 1, count);
            return false;
        }
        if (path.empty() || !index.emplace(path, range).second) {
            error = std::format("entry {}: empty or duplicate path '{}'", i + 1, path);
            return false;
        }
        if (range.offset > bytes.size() || range.size > bytes.size() - range.offset) {
            error = std::format("entry '{}' lies past the end of the file", path);
            return false;
        }
    }
    bytes_ = std::move(bytes);
    index_ = std::move(index);
    return true;
}

std::optional<std::string_view> Pak::find(const std::string& path) const
{
    const auto it = index_.find(path);
    if (it == index_.end())
        return std::nullopt;
    return std::string_view(bytes_).substr(static_cast<size_t>(it->second.offset), static_cast<size_t>(it->second.size));
}

std::vector<std::string> Pak::list(const std::string& dir) const
{
    const std::string prefix = dir.empty() ? "" : dir + "/";
    std::vector<std::string> paths;
    for (auto it = index_.lower_bound(prefix); it != index_.end() && it->first.starts_with(prefix); ++it)
        paths.push_back(it->first);
    return paths;
}

bool writePak(const std::filesystem::path& root, const std::vector<std::string>& paths, const std::filesystem::path& out,
              std::string& error)
{
    std::string header(kMagic, sizeof(kMagic));
    put(header, kPakVersion);
    put(header, static_cast<uint32_t>(paths.size()));
    size_t indexSize = header.size();
    for (const std::string& path : paths)
        indexSize += sizeof(uint32_t) + path.size() + 2 * sizeof(uint64_t);
    std::string readError;
    // Offsets are known only once each file is read, so the index is a placeholder that is filled in last. Only the
    // index and one file at a time are held in memory.
    const bool written = writeFileAtomic(out, [&](std::ostream& file) {
        file << header << std::string(indexSize - header.size(), '\0');
        std::string index;
        uint64_t offset = indexSize;
        for (const std::string& path : paths) {
            std::string bytes;
            if (!readFile(root / path, bytes)) {
                readError = std::format("Cannot read '{}'", (root / path).string());
                return false;
            }
            put(index, static_cast<uint32_t>(path.size()));
            index += path;
            put(index, offset);
            put(index, static_cast<uint64_t>(bytes.size()));
            file << bytes;
            offset += bytes.size();
        }
        file.seekp(static_cast<std::streamoff>(header.size()));
        file << index;
        return true;
    });
    if (!written) {
        error = readError.empty() ? std::format("Cannot write '{}'", out.string()) : readError;
        return false;
    }
    return true;
}

} // namespace kast
