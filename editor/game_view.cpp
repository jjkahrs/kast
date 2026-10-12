#include "game_view.h"

#include <imgui.h>

void GameViewport::shutdown()
{
    target_.destroy();
    renderer_.shutdown();
}

void GameViewport::draw(Document& doc)
{
    const bool visible = beginViewportPanel("Game", doc, PanelFocus::Game);
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const entt::entity camera = kast::activeCamera(doc.scene);
    if (visible && camera == entt::null) {
        const char* text = "No Camera in scene";
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(start.x + (size.x - textSize.x) * 0.5f, start.y + (size.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", text);
    } else if (visible) {
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        target_.resize(static_cast<uint16_t>(size.x * scale.x), static_cast<uint16_t>(size.y * scale.y));
        if (target_.valid() && size.x > 0.0f && size.y > 0.0f) {
            float view[16], proj[16];
            kast::cameraMatrices(doc.scene, camera, size.x / size.y, bgfx::getCaps()->homogeneousDepth, view, proj);
            renderer_.render(doc.scene, target_.view(), view, proj, false);
            ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
        }
    }
    ImGui::End();
}
