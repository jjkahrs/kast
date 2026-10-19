// Runs a program on a background thread (Bake Navmesh's kast_mapbake), collecting its stdout and stderr lines and its
// exit code, so the UI keeps drawing while it works. Windows only. No window of its own (CREATE_NO_WINDOW).
#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace zone {

class ProcessRunner {
public:
    ProcessRunner() = default;
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;
    ~ProcessRunner(); // waits for the program to end

    // Starts exe with args (each quoted). false with err set if it cannot start or one is already running.
    bool start(const std::filesystem::path& exe, const std::vector<std::filesystem::path>& args, std::string& err);
    bool running() const { return running_; }
    // The output lines since the last call.
    std::vector<std::string> takeLines();
    // Once it has ended.
    std::optional<int> exitCode() const;

private:
    std::jthread thread_;
    std::atomic<bool> running_ = false;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::optional<int> exitCode_;
};

// The kast_mapbake to run: setting if it names a file, else the first of KAST_SOURCE_DIR/services/world/build/
// {release,debug}/kast_mapbake.exe and next to this program that exists; empty if none.
std::filesystem::path find_mapbake(const std::string& setting);

} // namespace zone
