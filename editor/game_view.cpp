#include "game_view.h"

#include <imgui.h>

void GameViewport::shutdown()
{
    gui_.clear();
    gui_.context().release();
    target_.destroy();
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

void GameViewport::draw(Document& doc)
{
    syncGui(doc);
    const bool visible = beginViewportPanel("Game", doc, PanelFocus::Game);
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const entt::entity camera = kast::activeCamera(doc.scene);
    if (visible && size.x > 0.0f && size.y > 0.0f) {
        pixelScale_ = ImGui::GetIO().DisplayFramebufferScale.x;
        const auto width = static_cast<uint16_t>(size.x * pixelScale_);
        const auto height = static_cast<uint16_t>(size.y * ImGui::GetIO().DisplayFramebufferScale.y);
        target_.resize(width, height);
        if (target_.valid()) {
            if (camera != entt::null) {
                float view[16], proj[16];
                kast::cameraMatrices(doc.scene, camera, size.x / size.y, bgfx::getCaps()->homogeneousDepth, view, proj);
                renderer_.render(doc.scene, doc.project ? doc.project->root : std::filesystem::path(), target_.view(), view, proj,
                                 false);
            } else {
                bgfx::touch(target_.view()); // just the clear, under any GUI
            }
            if (doc.play != PlayState::Stopped) {
                gui_.context().resize(width, height);
                gui_.context().render(guiView_, target_.frameBuffer());
            }
            imageMin_ = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
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
