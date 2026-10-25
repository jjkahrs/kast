#include "model_preview.h"

#include "assets.h"
#include "ui_common.h"

#include <animation.h>
#include <files.h>
#include <log.h>

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

namespace {

const EditorCamera kStartCamera{.yaw = 20.0f, .pitch = -15.0f}; // then focus() on the model's bounds

} // namespace

void ModelPreview::init()
{
    renderer_.init();
    post_.init();
    // As the Shader Preview's: a sky behind the model, a sun from the upper left behind the camera.
    scene_.sky.mode = kast::SkyMode::Procedural;
    scene_.lighting = {.ambientColor = {0.7f, 0.8f, 1.0f}, .ambientIntensity = 0.3f};
    const entt::entity sun = scene_.create("Sun");
    scene_.registry().get<kast::Transform>(sun).rotation = {-0.408218f, -0.23457f, -0.109382f, 0.875426f};
    scene_.registry().emplace<kast::DirectionalLight>(sun, kast::DirectionalLight{.color = {1.0f, 0.95f, 0.9f},
                                                                                   .intensity = 1.1f,
                                                                                   .castShadows = false});
    object_ = scene_.create("Preview");
    scene_.registry().emplace<kast::ModelRenderer>(object_, kast::ModelRenderer{.castShadows = false, .receiveShadows = false});
    initialized_ = true;
}

void ModelPreview::shutdown()
{
    if (!initialized_)
        return;
    target_.destroy();
    post_.shutdown();
    renderer_.shutdown();
    initialized_ = false;
}

void ModelPreview::open(const std::string& path)
{
    path_ = path;
    shown_.reset();
    clip_.clear();
    playing_ = true;
    open_ = true;
    focus_ = true;
}

void ModelPreview::remap(const std::string& from, const std::string& to)
{
    if (std::vector<std::string> paths = {path_}; !path_.empty() && remapProjectPaths(paths, from, to))
        path_ = paths[0]; // the same file: the shown model, clip and camera stay
}

void ModelPreview::show(const std::shared_ptr<const kast::Model>& model)
{
    const bool fresh = !shown_;
    shown_ = model;
    lodEdit_ = model->lod;
    if (!initialized_)
        init();
    if (fresh) {
        camera_ = kStartCamera;
        camera_.focus(model->bounds);
    }
    int clip = kast::findClip(*model, clip_);
    if (clip < 0) {
        const std::string fallback = kast::defaultClip(*model);
        if (!clip_.empty())
            kast::log::warn(std::format("Model Preview: '{}' has no clip '{}'; playing '{}'", path_, clip_, fallback));
        clip_ = fallback;
        clip = kast::findClip(*model, clip_);
    }
    scene_.registry().get<kast::ModelRenderer>(object_).model = path_;
    // A reload's indices and pose sizes may have changed: start the state over.
    kast::AnimatorState& state = scene_.registry().emplace_or_replace<kast::AnimatorState>(object_);
    state.model = model.get();
    kast::playClip(state, clip, 0.0f);
}

void ModelPreview::draw(float dt)
{
    if (!open_)
        return;
    if (std::exchange(focus_, false))
        ImGui::SetNextWindowFocus();
    if (ImGui::Begin(kWindow, &open_)) {
        ImGui::TextUnformatted(path_.c_str());
        // ponytail: one stat per frame while open, as ModelLibrary::reloadChanged does per model.
        const std::filesystem::path root = kast::models().root();
        if (std::error_code ec; root.empty() || !std::filesystem::exists(root / fromUtf8(path_), ec)) {
            shown_.reset(); // deleted (or never there): nothing shows, a file put back there is framed anew
            ImGui::TextColored(kErrorColor, "The file doesn't exist.");
        } else if (const std::shared_ptr<const kast::Model> model = kast::models().get(path_); !model) {
            ImGui::TextColored(kErrorColor, "Can't load this model; the Console says why.");
        } else {
            if (model != shown_)
                show(model);
            drawImage(*model, dt);
        }
    }
    ImGui::End();
}

void ModelPreview::drawImage(const kast::Model& model, float dt)
{
    const kast::ModelStats stats = kast::modelStats(model);
    ImGui::TextDisabled("Meshes %zu  Vertices %zu  Triangles %zu  Nodes %zu  Bones %zu  Clips %zu", stats.meshes,
                        stats.vertices, stats.triangles, stats.nodes, stats.bones, stats.clips);
    ImGui::TextDisabled("Size %.2f x %.2f x %.2f m", stats.size.x, stats.size.y, stats.size.z);
    drawLod(model, stats);
    if (ImGui::Button("Reset view")) {
        camera_ = kStartCamera;
        camera_.focus(model.bounds);
    }
    kast::AnimatorState& state = scene_.registry().get<kast::AnimatorState>(object_);
    ImGui::SameLine();
    if (model.clips.empty()) {
        ImGui::TextDisabled("No animations");
    } else {
        if (ImGui::Button(playing_ ? "Pause" : "Play"))
            playing_ = !playing_;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##clip", clip_.c_str())) {
            for (size_t i = 0; i < model.clips.size(); ++i) {
                const bool selected = static_cast<int>(i) == state.clip;
                if (ImGui::Selectable(model.clips[i].name.c_str(), selected) && !selected) {
                    clip_ = model.clips[i].name;
                    kast::playClip(state, static_cast<int>(i), 0.0f);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    state.playing = playing_;
    kast::advanceAnimator(state, model, true, 1.0f, dt);

    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const auto pixels = [](float v) { return static_cast<uint16_t>(std::clamp(v, 0.0f, 65535.0f)); };
    const uint16_t width = pixels(size.x * scale.x), height = pixels(size.y * scale.y);
    post_.begin(width, height, kast::sceneClearColor(scene_.postProcessing, kViewportClearColor));
    target_.resize(width, height, post_.depth());
    if (!target_.valid() || size.x <= 0.0f || size.y <= 0.0f)
        return;
    ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            camera_.orbit(io.MouseDelta.x, io.MouseDelta.y);
        if (io.MouseWheel != 0.0f)
            camera_.dolly(io.MouseWheel);
    }
    float view[16], proj[16];
    camera_.viewMatrix(view);
    camera_.projMatrix(proj, size.x / size.y);
    renderer_.setForcedLod(lod_);
    renderer_.setTarget(post_.target(), post_.width(), post_.height(), post_.blendView());
    // No shadows (the light casts none), so no shadow views: the scene's view stands in for them.
    renderer_.render(scene_, kast::models().root(), post_.sceneView(), view, proj, post_.sceneView());
    post_.apply(scene_.postProcessing, false, target_.frameBuffer());
}

void ModelPreview::drawLod(const kast::Model& model, const kast::ModelStats& stats)
{
    if (!ImGui::CollapsingHeader("LOD", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    lod_ = std::clamp(lod_, 0, static_cast<int>(stats.lodTriangles.size()) - 1);
    kast::ModelLodSettings& edit = lodEdit_;
    ImGui::Checkbox("Auto LOD", &edit.autoLod);
    ImGui::SetItemTooltip("Generate levels for meshes without authored <name>_LOD<n> ones");
    ImGui::SameLine();
    int levels = static_cast<int>(edit.ratios.size());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::SliderInt("Levels", &levels, 1, static_cast<int>(kast::kMaxModelLods))) {
        // A new level halves the last one's triangles and screen size.
        while (edit.ratios.size() < static_cast<size_t>(levels)) {
            edit.ratios.push_back(edit.ratios.back() * 0.5f);
            edit.thresholds.push_back(edit.thresholds.back() * 0.5f);
        }
        edit.ratios.resize(static_cast<size_t>(levels));
        edit.thresholds.resize(static_cast<size_t>(levels));
    }
    ImGui::SameLine();
    ImGui::Checkbox("Cull below last", &edit.cullBelowLast);
    ImGui::SetItemTooltip("Don't draw the model when it covers less of the screen height than the last level's size");
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame;
    if (ImGui::BeginTable("##lods", 4, kFlags)) {
        ImGui::TableSetupColumn("Preview");
        ImGui::TableSetupColumn("Triangles (target %)");
        ImGui::TableSetupColumn("Screen height %");
        ImGui::TableSetupColumn("Triangles (imported)");
        ImGui::TableHeadersRow();
        const size_t rows = std::max(edit.ratios.size(), stats.lodTriangles.size());
        for (size_t level = 0; level < rows; ++level) {
            ImGui::PushID(static_cast<int>(level));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (level < stats.lodTriangles.size())
                ImGui::RadioButton(std::format("LOD{}", level).c_str(), &lod_, static_cast<int>(level));
            else
                ImGui::TextDisabled("LOD%zu", level);
            ImGui::TableNextColumn();
            if (level < edit.ratios.size()) {
                float percent = edit.ratios[level] * 100.0f;
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::BeginDisabled(level == 0);
                if (ImGui::DragFloat("##ratio", &percent, 0.5f, 1.0f, 100.0f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp))
                    edit.ratios[level] = percent / 100.0f;
                ImGui::EndDisabled();
            }
            ImGui::TableNextColumn();
            if (level < edit.thresholds.size()) {
                float percent = edit.thresholds[level] * 100.0f;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##threshold", &percent, 0.25f, 0.0f, 100.0f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp))
                    edit.thresholds[level] = percent / 100.0f;
            }
            ImGui::TableNextColumn();
            if (level < stats.lodTriangles.size())
                ImGui::Text("%zu (%.1f%%)", stats.lodTriangles[level],
                            100.0 * static_cast<double>(stats.lodTriangles[level]) /
                                static_cast<double>(std::max<size_t>(stats.lodTriangles[0], 1)));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const bool changed = edit != model.lod;
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Apply")) {
        const std::string file = kast::modelImportSettingsPath(path_);
        if (kast::writeFileAtomic(kast::models().root() / fromUtf8(file), kast::modelImportSettingsJson(edit)))
            kast::log::info(std::format("Model Preview: wrote '{}'; re-importing '{}'", file, path_));
        else
            kast::log::error(std::format("Model Preview: can't write '{}'", file));
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        edit = model.lod;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(changed ? "Apply re-imports the model with these settings" : "Settings as imported");
}
