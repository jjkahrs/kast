#pragma once

#include "document.h"

#include <post_process.h>
#include <renderer.h>

#include <bgfx/bgfx.h>

inline constexpr uint32_t kViewportClearColor = 0x2b3440ff; // Scene and Game panel background (sRGB)

// Offscreen color + depth target bound to one bgfx view, recreated when the panel size changes.
class ViewTarget {
public:
    ViewTarget(bgfx::ViewId view, const char* name) : view_(view), name_(name) {}

    // No-op if unchanged or empty. Without depth: its own depth, and the view clears to kClearColor. With depth
    // (PostProcess::depth, not owned): attaches that one and doesn't clear, for a post pass to fill and editor
    // overlays to draw over, depth-tested against the scene.
    void resize(uint16_t width, uint16_t height, bgfx::TextureHandle depth = BGFX_INVALID_HANDLE);
    void destroy();
    bgfx::ViewId view() const { return view_; }
    bool valid() const { return bgfx::isValid(frameBuffer_); }
    bgfx::TextureHandle texture() const { return bgfx::getTexture(frameBuffer_); }
    bgfx::FrameBufferHandle frameBuffer() const { return frameBuffer_; }

private:
    bgfx::ViewId view_;
    const char* name_;
    bgfx::FrameBufferHandle frameBuffer_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth_ = BGFX_INVALID_HANDLE; // the shared depth, if any
    uint16_t width_ = 0;
    uint16_t height_ = 0;
};

// Begins a borderless viewport panel window (call ImGui::End after, whatever it returns). Brings it to the front
// while doc.focusRequest names it, and clears the request once the panel shows focused. Returns whether it's visible.
bool beginViewportPanel(const char* name, Document& doc, PanelFocus panel);

// The Scene panel: renders the document through kast::Renderer and kast::PostProcess (the toolbar's "Post" toggles
// the effects) into an offscreen target, editor overlays over that, and handles the editor camera, click selection,
// transform gizmo and viewport shortcuts. Input is only taken while the panel is hovered (or a drag that started
// there is still held), so other panels keep theirs. Uses views first .. first + PostProcess::kViews (overlays last).
class SceneViewport {
public:
    explicit SceneViewport(bgfx::ViewId first) : post_(first), target_(first + kast::PostProcess::kViews, "Scene") {}

    void init();
    void shutdown();
    void draw(Document& doc);
    void setPostEffects(bool on) { postEffects_ = on; } // the toolbar's "Post" toggle

private:
    enum class Drag { None, Fly, Orbit, Pan };

    void drawToolbar(Document& doc);
    void updateCamera(Document& doc, bool hovered, float viewportHeight);
    void manipulate(Document& doc, const float* view, const float* proj);
    void pick(Document& doc, const float* viewProj, float x, float y, float width, float height);
    void shortcuts(Document& doc);

    kast::PostProcess post_;
    ViewTarget target_;
    kast::Renderer renderer_;
    bool postEffects_ = true;
    Drag drag_ = Drag::None;
    int operation_ = 0; // ImGuizmo::OPERATION; int so this header stays ImGuizmo-free
    bool local_ = true;
};
