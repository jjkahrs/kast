#pragma once

#include "document.h"

#include <renderer.h>

#include <bgfx/bgfx.h>

// The Scene panel: renders the document through kast::Renderer into an offscreen target and handles the
// editor camera, click selection, transform gizmo and viewport shortcuts. Input is only taken while the panel
// is hovered (or a drag that started there is still held), so other panels keep theirs.
class SceneViewport {
public:
    explicit SceneViewport(bgfx::ViewId view) : view_(view) {}

    void init();
    void shutdown();
    void draw(Document& doc);

private:
    enum class Drag { None, Fly, Orbit, Pan };

    void resize(uint16_t width, uint16_t height);
    void drawToolbar();
    void updateCamera(Document& doc, bool hovered, float viewportHeight);
    void manipulate(Document& doc, const float* view, const float* proj);
    void pick(Document& doc, const float* viewProj, float x, float y, float width, float height);
    void shortcuts(Document& doc);

    bgfx::ViewId view_;
    kast::Renderer renderer_;
    bgfx::FrameBufferHandle frameBuffer_ = BGFX_INVALID_HANDLE;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    Drag drag_ = Drag::None;
    int operation_ = 0; // ImGuizmo::OPERATION; int so this header stays ImGuizmo-free
    bool local_ = true;
};
