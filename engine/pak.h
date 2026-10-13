#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// game.pak: a built game's project content in one uncompressed archive, read by kast_player.
//
// Byte layout (all integers little-endian, offsets from the start of the file):
//   0   char[4]  magic "KPAK"
//   4   u32      format version (kPakVersion)
//   8   u32      entry count N
//   12  N index entries, back to back:
//         u32       path length L (> 0)
//         char[L]   path: project-relative, '/'-separated, UTF-8, no terminator; unique
//         u64       data offset
//         u64       data size
//   ... file data: each entry's bytes at [offset, offset + size), which must lie inside the file
namespace kast {

constexpr uint32_t kPakVersion = 1;

class Pak {
public:
    // Reads and validates the whole file. Replaces this pak only on success; on failure error says why.
    // ponytail: keeps the whole pak in memory; seek into the file instead if games grow past a few hundred MB.
    bool open(const std::filesystem::path& file, std::string& error);
    // The same, from bytes already in memory.
    bool parse(std::string bytes, std::string& error);

    // An entry's bytes, or nullopt if there is no such path. Valid while the pak lives.
    std::optional<std::string_view> find(const std::string& path) const;
    // Every path under dir ("" = all), sorted.
    std::vector<std::string> list(const std::string& dir) const;

private:
    struct Range {
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    std::string bytes_;
    std::map<std::string, Range> index_;
};

// Writes a pak holding root / each path (project-relative, '/'-separated) to out. False + error if a file can't be
// read or out can't be written.
bool writePak(const std::filesystem::path& root, const std::vector<std::string>& paths, const std::filesystem::path& out,
              std::string& error);

} // namespace kast
