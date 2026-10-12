#include "imgui_bgfx.h"

#include <app.h>
#include <log.h>

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h> // DockBuilder API

#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr bgfx::ViewId kSceneView = 1;
constexpr bgfx::ViewId kImGuiView = 255; // last, so the UI draws over everything
constexpr uint32_t kSceneClearColor = 0x3070b0ff;
constexpr uint32_t kBackbufferClearColor = 0x202020ff;
constexpr int kScreenshotTimeoutFrames = 30;

struct Options {
    int frames = 0;         // 0 = run until quit
    std::string screenshot; // captured after the last frame
    int width = 1280;
    int height = 720;
};

bool parseInt(std::string_view text, int& out)
{
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end && out > 0;
}

std::optional<Options> parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--frames" && hasValue && parseInt(argv[i + 1], options.frames)) {
            ++i;
        } else if (arg == "--screenshot" && hasValue) {
            options.screenshot = argv[++i];
        } else if (arg == "--size" && hasValue) {
            const std::string_view size = argv[++i];
            const size_t x = size.find('x');
            if (x == std::string_view::npos || !parseInt(size.substr(0, x), options.width) ||
                !parseInt(size.substr(x + 1), options.height)) {
                kast::log::error(std::format("Invalid --size '{}', expected WxH", size));
                return std::nullopt;
            }
        } else {
            kast::log::error(std::format(
                "Invalid argument '{}'. Usage: kast_editor [--frames N] [--screenshot out.png] [--size WxH]", arg));
            return std::nullopt;
        }
    }
    if (!options.screenshot.empty() && options.frames == 0) {
        kast::log::error("--screenshot requires --frames N");
        return std::nullopt;
    }
    return options;
}

// Offscreen target the Scene panel displays; recreated whenever the panel size changes.
class SceneTarget {
public:
    void resize(uint16_t width, uint16_t height)
    {
        if ((width == width_ && height == height_) || width == 0 || height == 0)
            return;
        destroy();
        frameBuffer_ = bgfx::createFrameBuffer(width, height, bgfx::TextureFormat::BGRA8,
                                               BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        width_ = width;
        height_ = height;
        bgfx::setViewName(kSceneView, "Scene");
        bgfx::setViewFrameBuffer(kSceneView, frameBuffer_);
        bgfx::setViewRect(kSceneView, 0, 0, width, height);
        bgfx::setViewClear(kSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, kSceneClearColor);
        kast::log::info(std::format("Scene framebuffer resized to {}x{}", width, height));
    }

    void destroy()
    {
        if (bgfx::isValid(frameBuffer_))
            bgfx::destroy(frameBuffer_);
        frameBuffer_ = BGFX_INVALID_HANDLE;
        width_ = height_ = 0;
    }

    bool valid() const { return bgfx::isValid(frameBuffer_); }
    ImTextureID texture() const { return static_cast<ImTextureID>(bgfx::getTexture(frameBuffer_).idx); }

private:
    bgfx::FrameBufferHandle frameBuffer_ = BGFX_INVALID_HANDLE;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
};

void buildDefaultLayout(ImGuiID dockspace)
{
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.3f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderFinish(dockspace);
}

void drawMenuBar()
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit"))
                kast::requestQuit(); // same SDL_EVENT_QUIT path as the window X
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void drawPlaceholder(const char* name)
{
    ImGui::Begin(name);
    ImGui::End();
}

void drawScene(SceneTarget& scene)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Scene")) {
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        scene.resize(static_cast<uint16_t>(size.x * scale.x), static_cast<uint16_t>(size.y * scale.y));
        if (scene.valid()) {
            bgfx::touch(kSceneView);
            ImGui::Image(scene.texture(), size);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void drawConsole()
{
    if (ImGui::Begin("Console")) {
        ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        // ponytail: draws every line each frame; switch to ImGuiListClipper when logs get long.
        for (const std::string& line : kast::log::lines())
            ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> options = parseArgs(argc, argv);
    if (!options)
        return 2;
    kast::log::info("Kast editor starting");

    SceneTarget scene;
    int frame = 0;
    bool screenshotRequested = false;
    bool screenshotSaved = false;
    bool failed = false;

    kast::AppHooks hooks;
    hooks.init = [](SDL_Window* window) {
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();
        if (!ImGui_ImplSDL3_InitForOther(window)) {
            kast::log::error("ImGui SDL3 backend init failed");
            ImGui::DestroyContext();
            return false;
        }
        imgui_bgfx::init(kImGuiView);
        bgfx::setViewClear(kImGuiView, BGFX_CLEAR_COLOR, kBackbufferClearColor);
        kast::log::info("ImGui initialized (docking enabled)");
        return true;
    };
    hooks.event = [](const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); };
    hooks.frame = [&] {
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        drawMenuBar();
        const ImGuiID dockspace = ImGui::GetID("MainDockSpace");
        if (!ImGui::DockBuilderGetNode(dockspace)) // no saved layout in imgui.ini
            buildDefaultLayout(dockspace);
        ImGui::DockSpaceOverViewport(dockspace);

        drawPlaceholder("Hierarchy");
        drawPlaceholder("Inspector");
        drawScene(scene);
        drawPlaceholder("Assets");
        drawConsole(); // submitted after Assets so Console is the front tab on first run

        ImGui::Render();
        bgfx::touch(kImGuiView);
        imgui_bgfx::render(ImGui::GetDrawData());

        ++frame;
        if (options->frames == 0 || frame < options->frames)
            return;
        if (options->screenshot.empty()) {
            if (frame == options->frames)
                kast::requestQuit();
        } else if (!screenshotRequested) {
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, options->screenshot.c_str());
            screenshotRequested = true;
        } else if (frame == options->frames + kScreenshotTimeoutFrames) {
            kast::log::error("Screenshot timed out");
            failed = true;
            kast::requestQuit();
        }
    };
    hooks.screenshotSaved = [&](const char*, bool saved) {
        screenshotSaved = saved;
        kast::requestQuit();
    };
    hooks.shutdown = [&] {
        scene.destroy();
        imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        kast::log::info("Shutdown: ImGui");
    };

    const int code = kast::run({.title = "Kast Editor", .width = options->width, .height = options->height}, hooks);
    if (code != 0)
        return code;
    if (!options->screenshot.empty() && !screenshotSaved)
        failed = true;
    return failed ? 1 : 0;
}
