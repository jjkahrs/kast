#include "gui.h"

#include "files.h"
#include "gui_render.h"
#include "log.h"

#include "roboto_regular.h"

#include <RmlUi/Core.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>

namespace kast::gui {
namespace {

class System final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        const std::string line = std::format("RmlUi: {}", message);
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            ++problems;
            log::error(line);
            break;
        case Rml::Log::LT_WARNING:
            ++problems;
            log::warn(line);
            break;
        case Rml::Log::LT_INFO:
        case Rml::Log::LT_ALWAYS:
            log::info(line);
            break;
        default: // debug chatter
            break;
        }
        return true;
    }

    uint64_t problems = 0;
};

// Whole files through kast::readFile, so documents and sheets come from the pak in kast_player.
class Files final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override
    {
        auto file = std::make_unique<File>();
        if (!readFile(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), file->data))
            return 0;
        return reinterpret_cast<Rml::FileHandle>(file.release());
    }
    void Close(Rml::FileHandle handle) override { delete reinterpret_cast<File*>(handle); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override
    {
        File& file = *reinterpret_cast<File*>(handle);
        const size_t count = std::min(size, file.data.size() - file.position);
        std::memcpy(buffer, file.data.data() + file.position, count);
        file.position += count;
        return count;
    }
    bool Seek(Rml::FileHandle handle, long offset, int origin) override
    {
        File& file = *reinterpret_cast<File*>(handle);
        const long base = origin == SEEK_CUR ? static_cast<long>(file.position)
                          : origin == SEEK_END ? static_cast<long>(file.data.size())
                                               : 0;
        const long position = base + offset;
        if (position < 0 || position > static_cast<long>(file.data.size()))
            return false;
        file.position = static_cast<size_t>(position);
        return true;
    }
    size_t Tell(Rml::FileHandle handle) override { return reinterpret_cast<File*>(handle)->position; }

private:
    struct File {
        std::string data;
        size_t position = 0;
    };
};

struct Globals {
    System system;
    Files files;
    GuiRenderer renderer;
};
std::unique_ptr<Globals> g_globals;

} // namespace

bool init()
{
    if (g_globals)
        return true;
    auto globals = std::make_unique<Globals>();
    Rml::SetSystemInterface(&globals->system);
    Rml::SetFileInterface(&globals->files);
    Rml::SetRenderInterface(&globals->renderer);
    if (!Rml::Initialise()) {
        log::error("RmlUi failed to initialise: game GUIs will not show");
        return false;
    }
    globals->renderer.init();
    // The bytes outlive Rml::Shutdown (a static array).
    if (!Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(kRobotoRegularTtf), sizeof(kRobotoRegularTtf)}, "Roboto",
                           Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto, true))
        log::error("RmlUi could not load the bundled Roboto font: GUI text will not show");
    g_globals = std::move(globals);
    log::info("RmlUi initialized");
    return true;
}

void shutdown()
{
    if (!g_globals)
        return;
    Rml::Shutdown();
    g_globals->renderer.shutdown();
    g_globals.reset();
}

bool ready() { return g_globals != nullptr; }

uint64_t problemCount() { return g_globals ? g_globals->system.problems : 0; }

GuiRenderer* renderer() { return g_globals ? &g_globals->renderer : nullptr; }

void syncTextInput(SDL_Window* window, bool want)
{
    if (!window || want == SDL_TextInputActive(window))
        return;
    if (want)
        SDL_StartTextInput(window);
    else
        SDL_StopTextInput(window);
}

} // namespace kast::gui

namespace kast {
namespace {

Rml::Input::KeyIdentifier toRmlKey(SDL_Keycode key)
{
    using namespace Rml::Input;
    if (key >= SDLK_A && key <= SDLK_Z)
        return static_cast<KeyIdentifier>(KI_A + (key - SDLK_A));
    if (key >= SDLK_0 && key <= SDLK_9)
        return static_cast<KeyIdentifier>(KI_0 + (key - SDLK_0));
    switch (key) {
    case SDLK_RETURN: return KI_RETURN;
    case SDLK_ESCAPE: return KI_ESCAPE;
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_TAB: return KI_TAB;
    case SDLK_SPACE: return KI_SPACE;
    case SDLK_DELETE: return KI_DELETE;
    case SDLK_LEFT: return KI_LEFT;
    case SDLK_RIGHT: return KI_RIGHT;
    case SDLK_UP: return KI_UP;
    case SDLK_DOWN: return KI_DOWN;
    case SDLK_HOME: return KI_HOME;
    case SDLK_END: return KI_END;
    case SDLK_PAGEUP: return KI_PRIOR;
    case SDLK_PAGEDOWN: return KI_NEXT;
    default: return KI_UNKNOWN;
    }
}

int toRmlModifiers(SDL_Keymod mod)
{
    int out = 0;
    if (mod & SDL_KMOD_CTRL)
        out |= Rml::Input::KM_CTRL;
    if (mod & SDL_KMOD_SHIFT)
        out |= Rml::Input::KM_SHIFT;
    if (mod & SDL_KMOD_ALT)
        out |= Rml::Input::KM_ALT;
    return out;
}

int toRmlButton(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    default: return -1;
    }
}

} // namespace

Rml::Context* GuiContext::context()
{
    if (!context_ && gui::ready()) {
        context_ = Rml::CreateContext(name_, {width_, height_});
        if (!context_)
            log::error(std::format("RmlUi could not create the '{}' GUI context", name_));
    }
    return context_;
}

void GuiContext::release()
{
    if (context_ && gui::ready())
        Rml::RemoveContext(name_);
    context_ = nullptr;
}

void GuiContext::resize(int width, int height)
{
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    if (context_)
        context_->SetDimensions({width_, height_});
}

void GuiContext::render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer)
{
    Rml::Context* rml = context();
    GuiRenderer* renderer = gui::renderer();
    if (!rml || !renderer)
        return;
    renderer->begin(view, frameBuffer, width_, height_);
    rml->Update();
    rml->Render();
}

void GuiContext::onEvent(const SDL_Event& event, float originX, float originY, float scale)
{
    Rml::Context* rml = context();
    if (!rml)
        return;
    const int mods = toRmlModifiers(SDL_GetModState());
    const auto x = [&](float value) { return static_cast<int>((value - originX) * scale); };
    const auto y = [&](float value) { return static_cast<int>((value - originY) * scale); };
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        rml->ProcessMouseMove(x(event.motion.x), y(event.motion.y), mods);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (const int button = toRmlButton(event.button.button); button >= 0) {
            rml->ProcessMouseMove(x(event.button.x), y(event.button.y), mods);
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                rml->ProcessMouseButtonDown(button, mods);
            else
                rml->ProcessMouseButtonUp(button, mods);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        rml->ProcessMouseWheel(Rml::Vector2f(-event.wheel.x, -event.wheel.y), mods); // RmlUi: positive scrolls down
        break;
    case SDL_EVENT_KEY_DOWN:
        rml->ProcessKeyDown(toRmlKey(event.key.key), toRmlModifiers(event.key.mod));
        break;
    case SDL_EVENT_KEY_UP:
        rml->ProcessKeyUp(toRmlKey(event.key.key), toRmlModifiers(event.key.mod));
        break;
    case SDL_EVENT_TEXT_INPUT:
        rml->ProcessTextInput(Rml::String(event.text.text));
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        rml->ProcessMouseLeave();
        break;
    default:
        break;
    }
}

} // namespace kast
