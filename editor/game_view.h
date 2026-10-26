#pragma once

#include "document.h"
#include "stats_toggle.h"
#include "viewport.h"

#include <renderer.h>
#include <scene_gui.h>
#include <stats_overlay.h>
#include <window_display.h>

#include <imgui.h>

#include <optional>
#include <utility>

// The Game panel: renders the scene from its active Camera (kast::activeCamera) with its post-processing and no
// grid, gizmos or other editor overlays, live in edit mode too, and the scene's game GUIs (UIDocument, see
// scene_gui.h) over it in Play mode, not post-processed. Uses views first .. first + Renderer::kShadowViews +
// PostProcess::kViews - 1 (shadow maps, then the scene and post passes) and guiView .. guiView + gui::kViews - 1. Comes to the front when Play starts. While it is focused and playing, the host feeds SDL input to
// Document::input and onEvent, and the Scene panel's shortcuts don't apply (it isn't focused). F3 there shows the stats
// overlay (StatsToggle) at the image's bottom-left, in ImGui: bgfx debug text would land at the window's corner.
// The GUIs scale as in kast_player (gui_scale.h) from the project's "gui" settings and the editor window's display
// scale. Window > Game View simulates another resolution (the image is drawn at it, fitted in the panel) and display
// scale; a label on the image says so while one is on.

// kast.display in the editor's Play: setMode logs and records the mode (the editor window never changes and no user
// settings are written); mode() is the recorded one at the Game view's size, modes() the editor display's, and
// displayScale() the Game view's (the editor's, or the simulated one).
class SimulatedDisplay final : public kast::DisplayAccess {
public:
    void attach(SDL_Window* window) { real_.attach(window); }
    bool setMode(const kast::DisplayMode& mode) override;
    kast::DisplayMode mode() override;
    std::vector<kast::DisplayModeInfo> modes() override { return real_.modes(); }
    double displayScale() override { return scale_; }
    void reset() { recorded_ = {}; } // Play stopped
    void update(int width, int height, double scale) // the Game view's GUI size and display scale
    {
        width_ = width;
        height_ = height;
        scale_ = scale;
    }
    double editorScale() { return real_.displayScale(); }

private:
    kast::WindowDisplay real_; // the editor window: only asked, never changed
    kast::DisplayMode recorded_;
    int width_ = 0;
    int height_ = 0;
    double scale_ = 1.0;
};

class GameViewport {
public:
    GameViewport(bgfx::ViewId first, bgfx::ViewId guiView)
        : shadowViews_(first), post_(first + kast::Renderer::kShadowViews), target_(guiView, "Game"), guiView_(guiView)
    {
    }

    void init(SDL_Window* window)
    {
        renderer_.init();
        post_.init();
        display_.attach(window);
    }
    void shutdown();
    // shareGuiViews: the Scene view's WorldUI preview drew in guiView's range this frame (SceneViewport::drewGui); the
    // GUIs take the views after its.
    void draw(Document& doc, bool shareGuiViews);
    bool hasInput(const Document& doc) const { return focused_ && doc.play == PlayState::Playing; }
    // An SDL event for the game GUIs (call only while hasInput).
    void onEvent(const SDL_Event& event);
    kast::SceneGui& gui() { return gui_; } // Play-mode GUIs: hot reload, --gui-click
    SimulatedDisplay& display() { return display_; } // kast.display in Play
    // The simulated resolution (nullopt: the panel's size) and display scale (nullopt: the editor's); --game-resolution
    // and --game-display-scale set them, Window > Game View (drawSimulationMenu) too.
    void simulate(std::optional<std::pair<int, int>> resolution, std::optional<double> displayScale)
    {
        resolution_ = resolution;
        displayScale_ = displayScale;
    }
    void drawSimulationMenu(); // the Window > Game View submenu
    kast::Renderer& renderer() { return renderer_; } // --no-instancing, --no-culling
    // Where the image is drawn, for Input's cursor position (kast.input.mouseRay).
    kast::Input::View inputView() const { return {imageMin_.x, imageMin_.y, imageSize_.x, imageSize_.y}; }
    // Window position of the drawn image's center (--sim-mouse presses there).
    ImVec2 imageCenter() const { return {imageMin_.x + imageSize_.x * 0.5f, imageMin_.y + imageSize_.y * 0.5f}; }

private:
    void syncGui(Document& doc);
    void drawStats(const Document& doc) const;
    void drawSimulationLabel(int width, int height, double scale) const;

    bgfx::ViewId shadowViews_;
    kast::PostProcess post_;
    ViewTarget target_; // on guiView_: the post chain's output, which the GUIs draw over
    bgfx::ViewId guiView_;
    kast::Renderer renderer_;
    kast::SceneGui gui_{"game"};
    SimulatedDisplay display_;
    std::optional<std::pair<int, int>> resolution_; // simulated; nullopt: the panel's size
    std::optional<double> displayScale_;            // simulated; nullopt: the editor window's
    bool scaleStale_ = true; // the next Play start takes the project's scale settings
    uint32_t guiGeneration_ = 0; // the Document::playGeneration gui_ was loaded for
    ImVec2 imageMin_{0.0f, 0.0f}; // screen position of the drawn image, for GUI mouse positions
    ImVec2 imageSize_{0.0f, 0.0f};
    float pixelScale_ = 1.0f;
    bool focused_ = false;
    StatsToggle stats_;
    kast::FpsAverage fps_; // the editor's frame rate, shared with kast_player's overlay
};
