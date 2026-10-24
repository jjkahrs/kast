#pragma once

#include "document.h"

#include <post_process.h>
#include <renderer.h>

#include <bgfx/bgfx.h>
#include <imgui.h>

#include <functional>

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

// Drops the prefab (project-relative) on the Scene view along ray (world space): a new root instance where the ray first
// hits a mesh or model, else where it crosses y = 0, else 10 m along it; selected (dropPrefab). Edit mode only, so there
// is no streamed terrain to hit.
entt::entity dropPrefabOnView(Document& doc, const std::string& path, const kast::Ray& ray, std::string& error);
// The ray through (u, v) (0..1 from the top-left) of an aspect-wide Scene view of doc.camera.
kast::Ray editorCameraRay(const Document& doc, float u, float v, float aspect, bool homogeneousDepth);

// The Scene panel: renders the document through kast::Renderer and kast::PostProcess (the toolbar's "Post" toggles
// the effects) into an offscreen target, editor overlays over that, and handles the editor camera, click selection,
// transform gizmo and viewport shortcuts. Input is only taken while the panel is hovered (or a drag that started
// there is still held), so other panels keep theirs. Uses views first .. first + Renderer::kShadowViews +
// PostProcess::kViews: shadow maps, then the scene and post passes, overlays last.
class SceneViewport {
public:
    explicit SceneViewport(bgfx::ViewId first)
        : shadowViews_(first), post_(first + kast::Renderer::kShadowViews),
          target_(first + kast::Renderer::kShadowViews + kast::PostProcess::kViews, "Scene")
    {
    }

    void init();
    void shutdown();
    void draw(Document& doc);
    void setPostEffects(bool on) { postEffects_ = on; } // the toolbar's "Post" toggle
    kast::Renderer& renderer() { return renderer_; }     // --no-instancing, --no-culling
    // The toolbar's Debug menu (docs/editor/controls.html#debug-views), applied to the renderer each frame: tint by
    // LOD level, boxes of the draws occlusion culled, and a Hi-Z pyramid mip (-1: off) in the view's corner.
    struct DebugViews {
        bool lodTint = false;
        bool occludedBoxes = false;
        int hizMip = -1;
    };
    DebugViews& debugViews() { return debug_; } // --lod-tint, --show-occluded, --show-hiz
    void openDebugMenu() { openDebugMenu_ = true; } // --scene-debug-menu: kept open, for screenshots
    // Bakes each of probes (ReflectionProbe entities of doc.scene) with this view's renderer and views
    // (Renderer::bakeProbe: call between frames), writes it to kast::bakedProbePath next to the scene, sets its
    // bakedPath and marks the document dirty. Edit mode and a scene saved in the project only; failures are logged.
    void bakeProbes(Document& doc, const std::vector<entt::entity>& probes);
    // Drawn with the document's scene, lit by it (Renderer::render's extra): the open zone in edit mode. Null: none.
    void setExtraScene(const kast::Scene* scene) { extraScene_ = scene; }
    // Called each frame the scene image shows, before the gizmo, with the view-projection and the image's rect (ImGui
    // points), to draw over it with the window's draw list: the zone overlays.
    std::function<void(const float* viewProj, float x, float y, float width, float height)> overlay;
    // The Scene view image this frame, for the caller's own mouse handling: viewProj, the image's rect (ImGui points),
    // whether the mouse is over it, and whether a camera drag (right fly, middle pan, Alt + left orbit) holds the mouse.
    struct Input {
        const float* viewProj;
        float x, y, width, height;
        bool hovered;
        bool cameraDrag;
    };
    // Called each frame the scene image shows, before overlay: true when the left mouse belongs to the caller this frame
    // (the zone's active tool, a zone handle it hit or drags), so the gizmo is hidden and a click selects nothing. The
    // camera keeps its controls either way.
    std::function<bool(const Input&)> input;
    // The scene image's center in window points (--sim-mouse without --play).
    ImVec2 imageCenter() const { return imageCenter_; }

private:
    enum class Drag { None, Fly, Orbit, Pan };

    void drawToolbar();
    void updateCamera(Document& doc, bool hovered, float viewportHeight);
    void manipulate(Document& doc, const float* view, const float* proj);
    void pick(Document& doc, const float* viewProj, float x, float y, float width, float height);
    void shortcuts(Document& doc);

    bgfx::ViewId shadowViews_;
    kast::PostProcess post_;
    ViewTarget target_;
    kast::Renderer renderer_;
    bool postEffects_ = true;
    Drag drag_ = Drag::None;
    int operation_ = 0; // ImGuizmo::OPERATION; int so this header stays ImGuizmo-free
    bool local_ = true;
    const kast::Scene* extraScene_ = nullptr;
    ImVec2 imageCenter_{0.0f, 0.0f};
    DebugViews debug_;
    bool openDebugMenu_ = false;
};
