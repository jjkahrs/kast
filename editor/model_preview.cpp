#include "model_preview.h"

#include "assets.h"
#include "ui_common.h"

#include <animation.h>
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
    renderer_.setTarget(post_.target(), post_.width(), post_.height());
    // No shadows (the light casts none), so no shadow views: the scene's view stands in for them.
    renderer_.render(scene_, kast::models().root(), post_.sceneView(), view, proj, post_.sceneView());
    post_.apply(scene_.postProcessing, false, target_.frameBuffer());
}
