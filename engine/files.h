#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

// Content file access for the engine (project.kast, scenes, Lua systems, Lua component definitions). Reads the disk,
// or only a mounted pak (kast_player). Hot reload's modification times stay disk-only: under a pak they are never
// found, so nothing reloads.
namespace kast {

class Pak;

// Reads file's bytes into out. False if it can't be read (missing, a directory, or not in the mounted pak).
bool readFile(const std::filesystem::path& file, std::string& out);
// Replaces file with bytes via a sibling "<file>.tmp" renamed over it, so a failed write leaves the old file intact.
// Always writes to disk, even with a pak mounted.
bool writeFileAtomic(const std::filesystem::path& file, std::string_view bytes);
// Same, with write filling the temp file (it may seek); false from write aborts and keeps the old file.
bool writeFileAtomic(const std::filesystem::path& file, const std::function<bool(std::ostream&)>& write);
// Every regular file under dir, recursively, sorted. Empty if there is no such directory.
std::vector<std::filesystem::path> listFiles(const std::filesystem::path& dir);

// From now on, files are read only from pak, as if it were extracted at root (an absolute directory).
void mountPak(Pak pak, const std::filesystem::path& root);

} // namespace kast
