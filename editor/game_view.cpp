#include "game_view.h"

#include <imgui.h>
#include <log.h>

void GameViewport::shutdown()
{
    gui_.clear();
    gui_.context().release();
    target_.destroy();
    post_.shutdown();
    renderer_.shutdown();
}

void GameViewport::onEvent(const SDL_Event& event) { gui_.context().onEvent(event, imageMin_.x, imageMin_.y, pixelScale_); }

void GameViewport::syncGui(Document& doc)
{
    if (doc.play == PlayState::Stopped || guiGeneration_ != doc.playGeneration) {
        gui_.clear(); // Stop, a new Play session or a kast.loadScene switch
        guiGeneration_ = doc.playGeneration;
    }
    if (doc.play != PlayState::Stopped)
        gui_.sync(doc.scene, doc.project ? doc.project->root : std::filesystem::path(), doc.lua.bus());
}

void GameViewport::drawStats(const Document& doc) const
{
    constexpr float kPad = 4.0f;
    const std::string text = kast::statsOverlayText(fps_.fps(), doc.lua.world().rttMs(),
                                                    kast::viewGpuMs("VoxelGI", shadowViews_, kast::Renderer::kShadowViews));
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
        bgfx::setDebug(stats_.visible ? BGFX_DEBUG_PROFILER : BGFX_DEBUG_NONE); // the VoxelGI view's GPU time
    }
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const entt::entity camera = kast::activeCamera(doc.scene);
    if (visible && size.x > 0.0f && size.y > 0.0f) {
        pixelScale_ = ImGui::GetIO().DisplayFramebufferScale.x;
        const auto width = static_cast<uint16_t>(size.x * pixelScale_);
        const auto height = static_cast<uint16_t>(size.y * ImGui::GetIO().DisplayFramebufferScale.y);
        post_.begin(width, height, kast::sceneClearColor(doc.scene.postProcessing, kViewportClearColor));
        target_.resize(width, height, post_.depth());
        if (target_.valid()) {
            if (camera != entt::null) { // else just the clear, under any GUI
                float view[16], proj[16];
                kast::cameraMatrices(doc.scene, camera, size.x / size.y, bgfx::getCaps()->homogeneousDepth, view, proj);
                renderer_.setTarget(post_.target(), post_.width(), post_.height());
                renderer_.render(doc.scene, doc.project ? doc.project->root : std::filesystem::path(), post_.sceneView(),
                                 view, proj, shadowViews_);
            }
            post_.apply(doc.scene.postProcessing, true, target_.frameBuffer());
            if (doc.play != PlayState::Stopped) {
                gui_.context().resize(width, height);
                gui_.context().render(guiView_, target_.frameBuffer());
            }
            imageMin_ = ImGui::GetCursorScreenPos();
            imageSize_ = size;
            ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
            if (stats_.visible)
                drawStats(doc);
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
