#pragma once

#include <chrono>
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
// The rename is retried for up to a second (renameRetrying). Always writes to disk, even with a pak mounted.
bool writeFileAtomic(const std::filesystem::path& file, std::string_view bytes);
// Same, with write filling the temp file (it may seek); false from write aborts and keeps the old file.
bool writeFileAtomic(const std::filesystem::path& file, const std::function<bool(std::ostream&)>& write);
// Renames from to to (replacing it), retrying every 20 ms for up to maxWait while it fails: a virus scanner or indexer
// briefly holds a file it just saw written. False if it still fails then.
bool renameRetrying(const std::filesystem::path& from, const std::filesystem::path& to,
                    std::chrono::milliseconds maxWait = std::chrono::seconds(1));
// Removes the temp files a tool run left in dir (not recursive): those whose names start with ownPrefix (this process's
// from an earlier process with the same id: call it before this process makes any) and any older than maxAge.
void removeStaleTempFiles(const std::filesystem::path& dir, std::string_view ownPrefix, std::chrono::minutes maxAge);
// Every regular file under dir, recursively, sorted. Empty if there is no such directory.
std::vector<std::filesystem::path> listFiles(const std::filesystem::path& dir);

// Sends target to the Recycle Bin, without a prompt unless Windows can't recycle it (it then asks before deleting it
// for good). False + error if that fails or is cancelled.
bool recycleFile(const std::filesystem::path& target, std::string& error);

// Models and textures loaded since start and the time spent loading them (read, decode/import, GPU upload), for
// load-time measurements: the editor and player log assetLoadSummary (renderer.h) on exit.
struct AssetLoadStats {
    int models = 0;
    double modelSeconds = 0.0;
    int textures = 0; // a sky cube counts once
    double textureSeconds = 0.0;
};
AssetLoadStats& assetLoadStats();
// Adds one to count and the time from construction to destruction to seconds.
class LoadTimer {
public:
    LoadTimer(int& count, double& seconds) : count_(count), seconds_(seconds) {}
    ~LoadTimer()
    {
        ++count_;
        seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    LoadTimer(const LoadTimer&) = delete;
    LoadTimer& operator=(const LoadTimer&) = delete;

private:
    int& count_;
    double& seconds_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// From now on, files are read only from pak, as if it were extracted at root (an absolute directory).
void mountPak(Pak pak, const std::filesystem::path& root);
// A pak is mounted: this is the player, which reads only what Build cooked.
bool pakMounted();

} // namespace kast
