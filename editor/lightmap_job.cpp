#include "lightmap_job.h"

#include <lightmap.h>
#include <log.h>

#include <format>

bool LightmapJob::start(Document& doc, std::string& error)
{
    const std::string scenePath = projectScenePath(doc);
    if (doc.play != PlayState::Stopped || scenePath.empty()) {
        error = "lightmaps bake in edit mode, in a scene saved in the project";
        return false;
    }
    if (running()) {
        error = "a bake is already running";
        return false;
    }
    if (thread_.joinable())
        thread_.join();
    auto input = std::make_unique<kast::LightmapBakeInput>();
    if (!kast::gatherLightmapBake(doc.scene, doc.project->root, scenePath, *input, error))
        return false;
    for (const std::string& warning : input->warnings)
        kast::log::warn(std::format("Bake Lightmaps: {}", warning));
    input_ = std::move(input);
    scenePath_ = scenePath;
    progress_.fraction.store(0.0f);
    progress_.cancel.store(false);
    done_.store(false);
    logged_ = -1;
    const kast::SceneLightmaps& s = input_->settings;
    kast::log::info(std::format("Baking lightmaps: {} Static meshes, {} lights, {} terrain maps ({} texels/m, {} bounces, "
                                "{} samples, probes every {} m)",
                                input_->entities.size(), input_->lights.size(), input_->terrain.size(), s.texelsPerMeter,
                                s.bounces, s.samplesPerTexel, s.probeSpacing));
    thread_ = std::jthread([this] {
        ok_ = kast::bakeLightmaps(*input_, progress_, output_, error_);
        done_.store(true);
    });
    return true;
}

std::optional<float> LightmapJob::progress() const
{
    return running() ? std::optional(progress_.fraction.load()) : std::nullopt;
}

void LightmapJob::wait()
{
    if (thread_.joinable())
        thread_.join();
}

void LightmapJob::poll(Document& doc, uint64_t frame)
{
    if (!input_) // no bake pending
        return;
    if (!done_.load()) {
        if (const int step = int(progress_.fraction.load() * 10.0f); step > logged_) {
            logged_ = step;
            kast::log::info(std::format("Baking lightmaps: {}% (frame {})", step * 10, frame));
        }
        return;
    }
    if (thread_.joinable())
        thread_.join();
    input_.reset();
    if (!ok_) {
        if (error_ == "cancelled")
            kast::log::info("Lightmap bake cancelled; the previous bake is kept");
        else
            kast::log::error(std::format("Lightmap bake failed: {}", error_));
        return;
    }
    std::string error;
    if (!kast::writeLightmapBake(output_, error)) {
        kast::log::error(std::format("Lightmap bake failed: {}", error));
        return;
    }
    const std::string sidecar = kast::lightmapSidecarPath(scenePath_);
    kast::log::info(std::format("Baked lightmaps to '{}' in {:.2f} s: {} atlas pages, {} texels, {} probes, {} terrain "
                                "tiles",
                                sidecar, output_.seconds, output_.atlases, output_.texels, output_.probes,
                                output_.terrainTiles));
    output_ = {};
    if (projectScenePath(doc) != scenePath_ || doc.play != PlayState::Stopped) {
        kast::log::warn("Bake Lightmaps: the scene changed while baking; its lightmaps are written but not set");
        return;
    }
    doc.scene.lighting.lightmaps.bakedPath = sidecar;
    markDirty(doc);
}
