#pragma once

#include "document.h"

#include <voxel_gi.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Bake VoxelGI in the editor (a VoxelGI's Inspector Bake, Window > Lighting's Bake VoxelGI, --bake-voxelgi;
// docs/engine/voxelgi.html): gathers each volume on the main thread (kast::gatherVoxelGiBake), voxelizes them one after
// the other on a worker thread (kast::bakeVoxelGi) while the editor keeps drawing and editing, and once all are done
// writes their files (each atomically, the .json last) and points each volume at its bake, marking the scene dirty.
// Cancel, a failure or a scene switch keeps the previous bakes as they were.
class VoxelGiJob {
public:
    ~VoxelGiJob() { cancel(); }
    // Starts a bake of volumes (VoxelGI entities of doc's scene; empty: all of them): edit mode, a scene saved in the
    // project, none running. False + error if not.
    bool start(Document& doc, std::vector<entt::entity> volumes, std::string& error);
    bool running() const { return thread_.joinable() && !done_.load(); }
    // 0..1 while running; nullopt when not.
    std::optional<float> progress() const;
    void cancel() { progress_.cancel.store(true); }
    // Blocks until the running bake ends (--bake-voxelgi).
    void wait();
    // Each frame: once the bake ended writes it (or logs why not).
    void poll(Document& doc);

private:
    std::vector<kast::VoxelGiBakeInput> inputs_;
    std::vector<kast::VoxelGiBakeOutput> outputs_;
    kast::LightmapBakeProgress progress_;
    std::atomic<size_t> current_{0}; // the input being baked
    std::jthread thread_;
    std::atomic<bool> done_{false};
    bool pending_ = false; // a bake started and not yet polled
    bool ok_ = false;
    std::string error_;
    std::string scenePath_; // project-relative: the scene it bakes
};
