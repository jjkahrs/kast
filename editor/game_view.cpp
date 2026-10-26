#include "game_view.h"

#include <imgui.h>
#include <log.h>

#include <algorithm>
#include <format>

namespace {

constexpr std::pair<int, int> kSimulatedResolutions[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr double kSimulatedScales[] = {1.0, 1.25, 1.5, 2.0};

} // namespace

bool SimulatedDisplay::setMode(const kast::DisplayMode& mode)
{
    if (mode.mode == kast::WindowMode::Exclusive && !kast::pickExclusiveMode(mode, modes())) {
        kast::log::warn(std::format("kast.display.setMode exclusive {}x{}: the display has no such mode (see "
                                    "kast.display.modes()); the mode is unchanged",
                                    mode.width, mode.height));
        return false;
    }
    recorded_ = mode;
    kast::log::info(std::format("kast.display.setMode {} {}x{}{}: recorded (the editor's Game view keeps its size; "
                                "kast_player switches)",
                                kast::windowModeName(mode.mode), mode.width, mode.height,
                                mode.refresh ? std::format("@{}", *mode.refresh) : ""));
    return true;
}

kast::DisplayMode SimulatedDisplay::mode()
{
    kast::DisplayMode out = recorded_;
    if (width_ > 0 && height_ > 0) {
        out.width = width_;
        out.height = height_;
    }
    return out;
}

void GameViewport::shutdown()
{
    gui_.release();
    target_.destroy();
    post_.shutdown();
    renderer_.shutdown();
}

void GameViewport::onEvent(const SDL_Event& event) { gui_.onEvent(event, imageMin_.x, imageMin_.y, pixelScale_); }

void GameViewport::syncGui(Document& doc)
{
    if (doc.play == PlayState::Stopped || guiGeneration_ != doc.playGeneration) {
        gui_.clear(); // Stop, a new Play session or a kast.loadScene switch
        guiGeneration_ = doc.playGeneration;
    }
    if (doc.play == PlayState::Stopped) {
        if (!scaleStale_)
            display_.reset();
        scaleStale_ = true; // kast.gui scale changes last the session, as kast_player's last the run
        return;
    }
    if (std::exchange(scaleStale_, false))
        gui_.setScale(doc.project ? doc.project->gui.scale : kast::GuiScale{});
    gui_.sync(doc.scene, doc.project ? doc.project->root : std::filesystem::path(), doc.lua.bus());
}

void GameViewport::drawSimulationMenu()
{
    if (!ImGui::BeginMenu("Game View"))
        return;
    ImGui::TextDisabled("Resolution");
    if (ImGui::MenuItem("Panel size", nullptr, !resolution_))
        resolution_.reset();
    for (const auto& [w, h] : kSimulatedResolutions)
        if (ImGui::MenuItem(std::format("{}x{}", w, h).c_str(), nullptr, resolution_ == std::pair{w, h}))
            resolution_ = std::pair{w, h};
    ImGui::Separator();
    ImGui::TextDisabled("Display scale");
    if (ImGui::MenuItem(std::format("Editor's ({:.0f}%)", display_.editorScale() * 100.0).c_str(), nullptr, !displayScale_))
        displayScale_.reset();
    for (const double scale : kSimulatedScales)
        if (ImGui::MenuItem(std::format("{:.0f}%", scale * 100.0).c_str(), nullptr, displayScale_ == scale))
            displayScale_ = scale;
    ImGui::EndMenu();
}

void GameViewport::drawSimulationLabel(int width, int height, double scale) const
{
    if (!resolution_ && !displayScale_)
        return;
    constexpr float kPad = 4.0f;
    const std::string text = std::format("Simulating {}x{} at {:.0f}% (Window > Game View)", width, height,
                                         scale * 100.0);
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min(imageMin_.x + imageSize_.x - textSize.x - 2.0f * kPad, imageMin_.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(imageMin_.x + imageSize_.x, min.y + textSize.y + 2.0f * kPad), IM_COL32(0, 0, 0, 160));
    draw->AddText(ImVec2(min.x + kPad, min.y + kPad), IM_COL32(0xff, 0xcc, 0x00, 0xff), text.c_str());
}

void GameViewport::drawStats(const Document& doc) const
{
    constexpr float kPad = 4.0f;
    const std::string text = kast::statsOverlayText(fps_.fps(), doc.lua.world().rttMs(),
                                                    kast::viewGpuMs("VoxelGI", shadowViews_, kast::Renderer::kShadowViews),
                                                    kast::viewGpuMs("SDFGI", shadowViews_, kast::Renderer::kShadowViews),
                                                    &renderer_.stats());
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min(imageMin_.x, imageMin_.y + imageSize_.y - textSize.y - 2.0f * kPad);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + textSize.x + 2.0f * kPad, imageMin_.y + imageSize_.y), IM_COL32(0, 0, 0, 160));
    draw->AddText(ImVec2(min.x + kPad, min.y + kPad), IM_COL32_WHITE, text.c_str());
}

void GameViewport::draw(Document& doc)
{
    syncGui(doc);
    const bool visible = beginViewportPanel("Game", doc, PanelFocus::Game);
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    fps_.addFrame(ImGui::GetIO().DeltaTime);
    const bool f3 = focused_ && ImGui::Shortcut(ImGuiKey_F3); // hardcoded, like Ctrl+P
    if (stats_.update(f3, focused_, doc.play)) {
        kast::log::info(stats_.visible ? "Stats overlay shown" : "Stats overlay hidden");
        bgfx::setDebug(stats_.visible ? BGFX_DEBUG_PROFILER : BGFX_DEBUG_NONE); // the VoxelGI and SDFGI views' GPU time
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const entt::entity camera = kast::activeCamera(doc.scene);
    if (visible && avail.x > 0.0f && avail.y > 0.0f) {
        // The image: the panel at its pixel size, or a simulated resolution fitted in the panel.
        ImVec2 size = avail;
        if (resolution_) {
            const float fit = std::min(avail.x / static_cast<float>(resolution_->first), avail.y / static_cast<float>(resolution_->second));
            size = ImVec2(resolution_->first * fit, resolution_->second * fit);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + (avail.x - size.x) * 0.5f,
                                             ImGui::GetCursorScreenPos().y + (avail.y - size.y) * 0.5f));
        }
        const auto width = static_cast<uint16_t>(resolution_ ? resolution_->first : size.x * ImGui::GetIO().DisplayFramebufferScale.x);
        const auto height = static_cast<uint16_t>(resolution_ ? resolution_->second : size.y * ImGui::GetIO().DisplayFramebufferScale.y);
        pixelScale_ = resolution_ ? static_cast<float>(width) / size.x : ImGui::GetIO().DisplayFramebufferScale.x;
        display_.update(width, height, displayScale_.value_or(display_.editorScale()));
        post_.begin(width, height, kast::sceneClearColor(doc.scene.postProcessing, kViewportClearColor),
                    doc.scene.postProcessing, true);
        target_.resize(width, height, post_.depth());
        if (target_.valid()) {
            if (camera != entt::null) { // else just the clear, under any GUI
                float view[16], proj[16];
                kast::cameraMatrices(doc.scene, camera, size.x / size.y, bgfx::getCaps()->homogeneousDepth, view, proj);
                post_.setProjection(proj);
                renderer_.setTarget(post_.target(), post_.width(), post_.height(), post_.blendView(),
                                    post_.volumetricFogView());
                gui_.world().layout(doc.scene, view, proj, width, height, &doc.lua.physics()); // none while Stopped
                renderer_.setWorldQuads(gui_.world().atlasTexture(), gui_.world().quads());
                renderer_.render(doc.scene, doc.project ? doc.project->root : std::filesystem::path(), post_.sceneView(),
                                 view, proj, shadowViews_);
            }
            post_.apply(doc.scene.postProcessing, true, target_.frameBuffer());
            if (doc.play != PlayState::Stopped) {
                gui_.setDisplayScale(display_.displayScale());
                gui_.resize(width, height);
                gui_.render(guiView_, target_.frameBuffer());
            }
            imageMin_ = ImGui::GetCursorScreenPos();
            imageSize_ = size;
            ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
            if (stats_.visible)
                drawStats(doc);
            drawSimulationLabel(width, height, display_.displayScale());
        }
        if (camera == entt::null) {
            const char* text = "No Camera in scene";
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            ImGui::GetWindowDrawList()->AddText(ImVec2(imageMin_.x + (size.x - textSize.x) * 0.5f,
                                                       imageMin_.y + (size.y - textSize.y) * 0.5f),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        }
    }
    ImGui::End();
}
