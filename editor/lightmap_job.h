#pragma once

#include "document.h"

#include <lightmap_bake.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

// Bake Lightmaps in the editor (Window > Lighting, --bake-lightmaps; docs/engine/lightmaps.html): gathers the scene on
// the main thread (kast::gatherLightmapBake), bakes it on a worker thread (kast::bakeLightmaps, on every core) while the
// editor keeps drawing and editing, and once it is done writes its files (kast::writeLightmapBake) and points the scene
// at them, marking it dirty. Cancel, a failure or a scene switch keeps the previous bake as it was.
class LightmapJob {
public:
    ~LightmapJob() { cancel(); }
    // Starts a bake of doc's scene: edit mode, a scene saved in the project, none running. False + error if not.
    bool start(Document& doc, std::string& error);
    bool running() const { return thread_.joinable() && !done_.load(); }
    // 0..1 while running; nullopt when not.
    std::optional<float> progress() const;
    // Asks the running bake to stop (it does within a chunk of texels); poll() then reports it.
    void cancel() { progress_.cancel.store(true); }
    // Blocks until the running bake ends (--bake-lightmaps).
    void wait();
    // Each frame: logs the progress at every 10% with the frame number (the editor draws meanwhile), and once the bake
    // ended writes it (or logs why not).
    void poll(Document& doc, uint64_t frame);

private:
    std::unique_ptr<kast::LightmapBakeInput> input_;
    kast::LightmapBakeProgress progress_;
    std::jthread thread_;
    std::atomic<bool> done_{false};
    bool ok_ = false;
    std::string error_;
    kast::LightmapBakeOutput output_;
    std::string scenePath_; // project-relative: the scene it bakes
    int logged_ = -1;       // the last 10% step logged
};
