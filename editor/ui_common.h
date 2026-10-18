#pragma once

#include <imgui.h>

#include <cstdint>
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
constexpr ImVec4 kOkColor{0.4f, 0.85f, 0.4f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};

// Explains why the last (disabled) item is disabled, if it is.
void disabledTooltip(const char* reason);

constexpr const char* kAssetPayload = "KAST_ASSET"; // drag and drop from the Assets panel: a project-relative path
// The last item as a drop target for a model file (.glb/.gltf/.fbx/.obj) dragged from the Assets panel: true and its
// path once dropped. Other assets are not accepted.
bool acceptModelDrop(std::string& path);
// The same for a .prefab.
bool acceptPrefabDrop(std::string& path);

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
// --screenshot-on-log D,TEXT moves that last frame up to D frames after the first log line containing TEXT (tests
// that wait for something in the game, such as a death).
class FrameLimit {
public:
    FrameLimit(int frames, std::string screenshot) : frames_(frames), screenshot_(std::move(screenshot)) {}
    void endOnLog(int delayFrames, std::string text)
    {
        logDelay_ = delayFrames;
        logText_ = std::move(text);
    }

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
    std::string logText_; // empty: no --screenshot-on-log
    int logDelay_ = 0;
    size_t logSeen_ = 0; // log lines already searched
    int frame_ = 0;
    bool requested_ = false;
    bool saved_ = false;
    bool failed_ = false;
};

// --log-fps: once a second, the frame rate (vsync caps it at the display's refresh rate) and what a frame costs below
// that cap: the app's CPU time per frame (update, UI and submitting draws) and bgfx's last GPU frame time.
class FpsLog {
public:
    // frameStart: SDL_GetPerformanceCounter() at the start of the frame.
    void endFrame(uint64_t frameStart);

private:
    uint64_t windowStart_ = 0;
    int frames_ = 0;
    double cpuSeconds_ = 0.0;
};
