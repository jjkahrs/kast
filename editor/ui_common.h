#pragma once

#include <imgui.h>
#include <stats_overlay.h>

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
// The embedded Roboto font at the base size (ImGui default font if it fails to load), with the icons below merged in.
void loadFont();
// Material Symbols icons in the editor font (UTF-8).
constexpr const char* kIconPlay = "\xee\x80\xb7";  // U+E037 play_arrow
constexpr const char* kIconPause = "\xee\x80\xb4"; // U+E034 pause
constexpr const char* kIconStop = "\xee\x81\x87";  // U+E047 stop
// The editor palette (see CLAUDE.md): every editor color comes from here.
constexpr ImU32 kAccent = IM_COL32(0xff, 0xa1, 0x00, 0xff);    // buttons, checks; text on tabs, headers, selection
constexpr ImU32 kHighlight = IM_COL32(0xff, 0xd0, 0x00, 0xff); // hovered button, links, viewport labels
constexpr ImU32 kPressed = IM_COL32(0xe0, 0x8d, 0x00, 0xff);   // pressed button
constexpr ImU32 kWarning = IM_COL32(0xff, 0xb7, 0x03, 0xff);
constexpr ImU32 kError = IM_COL32(0xe6, 0x39, 0x46, 0xff);
constexpr ImU32 kText = IM_COL32(0xf3, 0xf4, 0xf6, 0xff);
constexpr ImU32 kTextSecondary = IM_COL32(0x9e, 0x9e, 0xa8, 0xff); // disabled text, inactive tabs
constexpr ImU32 kBackground = IM_COL32(0x0c, 0x0c, 0x0e, 0xff);
constexpr ImU32 kCard = IM_COL32(0x15, 0x15, 0x18, 0xff);  // cards, popups, inactive tabs
constexpr ImU32 kField = IM_COL32(0x22, 0x22, 0x27, 0xff); // text fields, borders, active tab, headers, selection
constexpr ImU32 kRowHover = IM_COL32(0x1c, 0x1c, 0x20, 0xff);
constexpr ImU32 kFieldHighlight = IM_COL32(0x3a, 0x3a, 0x41, 0xff);
// Zone tints over the terrain in the Scene view (Map tab, Zones), one per zone in order, repeating: translucent and
// clear of the yellows/orange (those are for controls) and the error red.
constexpr ImU32 kZoneTints[] = {
    IM_COL32(0x3a, 0x86, 0xff, 0x70), // blue
    IM_COL32(0x2e, 0xc4, 0x8a, 0x70), // green
    IM_COL32(0x9b, 0x5d, 0xe5, 0x70), // purple
    IM_COL32(0x00, 0xbb, 0xd4, 0x70), // cyan
    IM_COL32(0xf1, 0x5b, 0xb5, 0x70), // pink
    IM_COL32(0x5c, 0x6b, 0xc0, 0x70), // indigo
};
constexpr ImU32 kZoneHoverLine = kHighlight; // the tile under the mouse while painting zones; the painted zone's label

// An IM_COL32 (ABGR) color as bgfx's RGBA, e.g. for a clear color.
constexpr uint32_t toRgba(ImU32 color)
{
    return ((color >> IM_COL32_R_SHIFT) & 0xff) << 24 | ((color >> IM_COL32_G_SHIFT) & 0xff) << 16 |
           ((color >> IM_COL32_B_SHIFT) & 0xff) << 8 | ((color >> IM_COL32_A_SHIFT) & 0xff);
}
constexpr ImU32 withAlpha(ImU32 color, unsigned alpha)
{
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}
constexpr ImVec4 toVec4(ImU32 color)
{
    const auto channel = [&](int shift) { return static_cast<float>((color >> shift) & 0xff) / 255.0f; };
    return {channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT), channel(IM_COL32_A_SHIFT)};
}
constexpr ImVec4 kOkColor{0.4f, 0.85f, 0.4f, 1.0f};
constexpr ImVec4 kErrorColor = toVec4(kError);

// The editor palette on top of ImGui's dark style. Call before UiScale::init.
void applyTheme();
// ImGui has one text color per style: recolors text by the fill it is drawn on. Black on the accent fills (buttons and
// other controls), the accent on tabs, headers and selected rows, secondary on inactive tabs. Call after ImGui::Render,
// before rendering the draw data.
void darkenTextOnAccent(ImDrawData* drawData);

// Explains why the last (disabled) item is disabled, if it is.
void disabledTooltip(const char* reason);

constexpr const char* kAssetPayload = "KAST_ASSET"; // drag and drop from the Assets panel: a project-relative path
// The last item as a drop target for a model file (.glb/.gltf/.fbx/.obj) dragged from the Assets panel: true and its
// path once dropped. Other assets are not accepted.
bool acceptModelDrop(std::string& path);
// The same for a .prefab.
bool acceptPrefabDrop(std::string& path);
// The same for a .guiprefab.
bool acceptGuiPrefabDrop(std::string& path);
// A small cube (top face and two sides) at x in a row from y, height tall: marks prefab instance roots in the Entities
// panel and GUI Hierarchy. Accent colored.
void drawPrefabIcon(float x, float y, float height);
// The kHighlight bar at the window's left edge beside an overridden prefab value, from y top to bottom (screen).
void markOverride(float top, float bottom);

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
// that cap: the app's CPU time per frame (update, UI and submitting draws) and bgfx's last GPU frame time; then a
// renderer's last camera-pass counts and Hi-Z latency, if given.
class FpsLog {
public:
    // frameStart: SDL_GetPerformanceCounter() at the start of the frame. scene: the Scene view's Renderer::stats,
    // hizLatency its occlusionLatency.
    void endFrame(uint64_t frameStart, const kast::RenderStats* scene = nullptr, int hizLatency = 0);

private:
    uint64_t windowStart_ = 0;
    int frames_ = 0;
    double cpuSeconds_ = 0.0;
};
