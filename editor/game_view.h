#pragma once

#include "document.h"
#include "viewport.h"

#include <renderer.h>
#include <scene_gui.h>

#include <imgui.h>

// The Game panel: renders the scene from its active Camera (kast::activeCamera) with no grid, gizmos or other
// editor overlays, live in edit mode too, and the scene's game GUIs (UIDocument, see scene_gui.h) over it in Play
// mode. Comes to the front when Play starts. While it is focused and playing, the host feeds SDL input to
// Document::input and onEvent, and the Scene panel's shortcuts don't apply (it isn't focused).
class GameViewport {
public:
    GameViewport(bgfx::ViewId view, bgfx::ViewId guiView) : target_(view, "Game"), guiView_(guiView) {}

    void init() { renderer_.init(); }
    void shutdown();
    void draw(Document& doc);
    bool hasInput(const Document& doc) const { return focused_ && doc.play == PlayState::Playing; }
    // An SDL event for the game GUIs (call only while hasInput).
    void onEvent(const SDL_Event& event);
    kast::SceneGui& gui() { return gui_; } // Play-mode GUIs: hot reload, --gui-click

private:
    void syncGui(Document& doc);

    ViewTarget target_;
    bgfx::ViewId guiView_;
    kast::Renderer renderer_;
    kast::SceneGui gui_{"game"};
    uint32_t guiGeneration_ = 0; // the Document::playGeneration gui_ was loaded for
    ImVec2 imageMin_{0.0f, 0.0f}; // screen position of the drawn image, for GUI mouse positions
    float pixelScale_ = 1.0f;
    bool focused_ = false;
};
