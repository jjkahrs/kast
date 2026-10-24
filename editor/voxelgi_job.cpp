#include "voxelgi_job.h"

#include <files.h>
#include <log.h>

#include <format>

bool VoxelGiJob::start(Document& doc, std::vector<entt::entity> volumes, std::string& error)
{
    const std::string scenePath = projectScenePath(doc);
    if (doc.play != PlayState::Stopped || scenePath.empty()) {
        error = "VoxelGI bakes in edit mode, in a scene saved in the project";
        return false;
    }
    if (running()) {
        error = "a bake is already running";
        return false;
    }
    if (thread_.joinable())
        thread_.join();
    if (volumes.empty())
        doc.scene.forEachDepthFirst([&](entt::entity entity) {
            if (doc.scene.registry().all_of<kast::VoxelGI>(entity))
                volumes.push_back(entity);
        });
    if (volumes.empty()) {
        error = "the scene has no VoxelGI volume";
        return false;
    }
    std::vector<kast::VoxelGiBakeInput> inputs;
    for (const entt::entity volume : volumes) {
        kast::VoxelGiBakeInput input;
        if (!kast::gatherVoxelGiBake(doc.scene, doc.project->root, scenePath, volume, input, error))
            return false;
        for (const std::string& warning : input.scene.warnings)
            kast::log::warn(std::format("Bake VoxelGI '{}': {}", input.name, warning));
        kast::log::info(std::format("Baking VoxelGI '{}': {} Static meshes, {} terrain maps, {}x{}x{} voxels of {:.3f} m",
                                    input.name, input.scene.entities.size(), input.scene.terrain.size(), input.grid.dims[0],
                                    input.grid.dims[1], input.grid.dims[2], input.grid.voxelSize));
        inputs.push_back(std::move(input));
    }
    inputs_ = std::move(inputs);
    outputs_.assign(inputs_.size(), {});
    scenePath_ = scenePath;
    progress_.fraction.store(0.0f);
    progress_.cancel.store(false);
    current_.store(0);
    done_.store(false);
    pending_ = true;
    thread_ = std::jthread([this] {
        ok_ = true;
        for (size_t i = 0; ok_ && i < inputs_.size(); ++i) {
            current_.store(i);
            ok_ = kast::bakeVoxelGi(inputs_[i], progress_, outputs_[i], error_);
        }
        done_.store(true);
    });
    return true;
}

std::optional<float> VoxelGiJob::progress() const
{
    if (!running())
        return std::nullopt;
    const size_t count = std::max<size_t>(inputs_.size(), 1);
    return (float(current_.load()) + progress_.fraction.load()) / float(count);
}

void VoxelGiJob::wait()
{
    if (thread_.joinable())
        thread_.join();
}

void VoxelGiJob::poll(Document& doc)
{
    if (!pending_ || !done_.load())
        return;
    if (thread_.joinable())
        thread_.join();
    pending_ = false;
    if (!ok_) {
        if (error_ == "cancelled")
            kast::log::info("VoxelGI bake cancelled; the previous bake is kept");
        else
            kast::log::error(std::format("VoxelGI bake failed: {}", error_));
        return;
    }
    for (size_t i = 0; i < inputs_.size(); ++i)
        for (const auto& [file, bytes] : outputs_[i].files) {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (!kast::writeFileAtomic(file, bytes)) {
                kast::log::error(std::format("VoxelGI bake failed: cannot write '{}'", file.string()));
                return;
            }
        }
    const bool same = projectScenePath(doc) == scenePath_ && doc.play == PlayState::Stopped;
    entt::registry& registry = doc.scene.registry();
    for (size_t i = 0; i < inputs_.size(); ++i) {
        const kast::VoxelGiBakeInput& input = inputs_[i];
        kast::log::info(std::format("Baked VoxelGI '{}' to '{}' in {:.2f} s: {} voxels", input.name, input.bakedPath,
                                    outputs_[i].seconds, outputs_[i].voxels));
        if (!same)
            continue;
        for (auto [entity, id, volume] : registry.view<const kast::PersistentId, kast::VoxelGI>().each())
            if (id.value == input.id) {
                volume.bakedPath = input.bakedPath;
                markDirty(doc);
            }
    }
    if (!same)
        kast::log::warn("Bake VoxelGI: the scene changed while baking; its bakes are written but not set");
    inputs_.clear();
    outputs_.clear();
}
