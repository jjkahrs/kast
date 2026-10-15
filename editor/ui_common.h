#pragma once

#include <imgui.h>

#include <optional>
#include <string>
#include <utility>

// ImGui setup and helpers shared by the editor and the Project Manager window.

union SDL_Event;
struct SDL_Window;

constexpr float kMinUiScale = 0.75f;
constexpr float kMaxUiScale = 3.0f;

float clampUiScale(float scale);
// The embedded Roboto font at the base size (ImGui default font if it fails to load).
void loadFont();
// The editor palette (see CLAUDE.md) on top of ImGui's dark style. Call before UiScale::init.
void applyTheme();
// ImGui has one text color, and white text on the accent fills (buttons, tabs, headers, selection) is unreadable: turns
// text drawn over an accent fill black. Call after ImGui::Render, before rendering the draw data.
void darkenTextOnAccent(ImDrawData* drawData);
// Explains why the last (disabled) item is disabled, if it is.
void disabledTooltip(const char* reason);

constexpr const char* kAssetPayload = "KAST_ASSET"; // drag and drop from the Assets panel: a project-relative path
// The last item as a drop target for a model file (.glb/.gltf/.fbx/.obj) dragged from the Assets panel: true and its
// path once dropped. Other assets are not accepted.
bool acceptModelDrop(std::string& path);

// A card: a rounded, tinted, bordered panel behind everything drawn between beginCard and endCard, spanning the window's
// width, with a gap below it. Cards do not nest. Its edges line up with collapsing headers, which cardHeader draws with
// corners rounded to match.
void beginCard();
void endCard();
bool cardHeader(const char* label, bool* open, ImGuiTreeNodeFlags flags = 0);

// Scales ImGui text and style metrics by the window's display scale, or a fixed --ui-scale override.
class UiScale {
public:
    explicit UiScale(std::optional<float> forced) : forced_(forced) {}

    // Call once the style is final; it is kept unscaled so rescaling never compounds.
    void init(SDL_Window* window);
    void onEvent(const SDL_Event& event);

private:
    void apply();
    float displayScale() const;

    std::optional<float> forced_;
    SDL_Window* window_ = nullptr;
    ImGuiStyle base_;
    float current_ = 0.0f;
};

// --frames N [--screenshot out.png]: quits after N frames, or captures a screenshot then and quits once it is saved.
class FrameLimit {
public:
    FrameLimit(int frames, std::string screenshot) : frames_(frames), screenshot_(std::move(screenshot)) {}

    // Call at the end of every frame, after ImGui::Render.
    void endFrame();
    // AppHooks::screenshotSaved.
    void onScreenshotSaved(bool saved);
    int frame() const { return frame_; }
    // After kast::run: the screenshot timed out or was not saved.
    bool failed() const { return failed_ || (!screenshot_.empty() && !saved_); }

private:
    int frames_ = 0; // 0 = run until quit
    std::string screenshot_;
    int frame_ = 0;
    bool requested_ = false;
    bool saved_ = false;
    bool failed_ = false;
};
