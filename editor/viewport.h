#pragma once

#include "document.h"

#include <renderer.h>

#include <bgfx/bgfx.h>

// Offscreen color + depth target bound to one bgfx view, recreated when the panel size changes.
class ViewTarget {
public:
    ViewTarget(bgfx::ViewId view, const char* name) : view_(view), name_(name) {}

    void resize(uint16_t width, uint16_t height); // no-op if unchanged or empty
    void destroy();
    bgfx::ViewId view() const { return view_; }
    bool valid() const { return bgfx::isValid(frameBuffer_); }
    bgfx::TextureHandle texture() const { return bgfx::getTexture(frameBuffer_); }
    bgfx::FrameBufferHandle frameBuffer() const { return frameBuffer_; }

private:
    bgfx::ViewId view_;
    const char* name_;
    bgfx::FrameBufferHandle frameBuffer_ = BGFX_INVALID_HANDLE;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
};

// Begins a borderless viewport panel window (call ImGui::End after, whatever it returns). Brings it to the front
// while doc.focusRequest names it, and clears the request once the panel shows focused. Returns whether it's visible.
bool beginViewportPanel(const char* name, Document& doc, PanelFocus panel);

// The Scene panel: renders the document through kast::Renderer into an offscreen target and handles the
// editor camera, click selection, transform gizmo and viewport shortcuts. Input is only taken while the panel
// is hovered (or a drag that started there is still held), so other panels keep theirs.
class SceneViewport {
public:
    explicit SceneViewport(bgfx::ViewId view) : target_(view, "Scene") {}

    void init();
    void shutdown();
    void draw(Document& doc);

private:
    enum class Drag { None, Fly, Orbit, Pan };

    void drawToolbar(Document& doc);
    void updateCamera(Document& doc, bool hovered, float viewportHeight);
    void manipulate(Document& doc, const float* view, const float* proj);
    void pick(Document& doc, const float* viewProj, float x, float y, float width, float height);
    void shortcuts(Document& doc);

    ViewTarget target_;
    kast::Renderer renderer_;
    Drag drag_ = Drag::None;
    int operation_ = 0; // ImGuizmo::OPERATION; int so this header stays ImGuizmo-free
    bool local_ = true;
};
