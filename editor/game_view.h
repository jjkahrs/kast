#pragma once

#include "document.h"
#include "stats_toggle.h"
#include "viewport.h"

#include <renderer.h>
#include <scene_gui.h>
#include <stats_overlay.h>

#include <imgui.h>

// The Game panel: renders the scene from its active Camera (kast::activeCamera) with its post-processing and no
// grid, gizmos or other editor overlays, live in edit mode too, and the scene's game GUIs (UIDocument, see
// scene_gui.h) over it in Play mode, not post-processed. Uses views first .. first + Renderer::kShadowViews +
// PostProcess::kViews - 1 (shadow maps, then the scene and post passes) and guiView .. guiView + gui::kViews - 1. Comes to the front when Play starts. While it is focused and playing, the host feeds SDL input to
// Document::input and onEvent, and the Scene panel's shortcuts don't apply (it isn't focused). F3 there shows the stats
// overlay (StatsToggle) at the image's bottom-left, in ImGui: bgfx debug text would land at the window's corner.
class GameViewport {
public:
    GameViewport(bgfx::ViewId first, bgfx::ViewId guiView)
        : shadowViews_(first), post_(first + kast::Renderer::kShadowViews), target_(guiView, "Game"), guiView_(guiView)
    {
    }

    void init()
    {
        renderer_.init();
        post_.init();
    }
    void shutdown();
    void draw(Document& doc);
    bool hasInput(const Document& doc) const { return focused_ && doc.play == PlayState::Playing; }
    // An SDL event for the game GUIs (call only while hasInput).
    void onEvent(const SDL_Event& event);
    kast::SceneGui& gui() { return gui_; } // Play-mode GUIs: hot reload, --gui-click
    kast::Renderer& renderer() { return renderer_; } // --no-instancing, --no-culling
    // Where the image is drawn, for Input's cursor position (kast.input.mouseRay).
    kast::Input::View inputView() const { return {imageMin_.x, imageMin_.y, imageSize_.x, imageSize_.y}; }
    // Window position of the drawn image's center (--sim-mouse presses there).
    ImVec2 imageCenter() const { return {imageMin_.x + imageSize_.x * 0.5f, imageMin_.y + imageSize_.y * 0.5f}; }

private:
    void syncGui(Document& doc);
    void drawStats(const Document& doc) const;

    bgfx::ViewId shadowViews_;
    kast::PostProcess post_;
    ViewTarget target_; // on guiView_: the post chain's output, which the GUIs draw over
    bgfx::ViewId guiView_;
    kast::Renderer renderer_;
    kast::SceneGui gui_{"game"};
    uint32_t guiGeneration_ = 0; // the Document::playGeneration gui_ was loaded for
    ImVec2 imageMin_{0.0f, 0.0f}; // screen position of the drawn image, for GUI mouse positions
    ImVec2 imageSize_{0.0f, 0.0f};
    float pixelScale_ = 1.0f;
    bool focused_ = false;
    StatsToggle stats_;
    kast::FpsAverage fps_; // the editor's frame rate, shared with kast_player's overlay
};
