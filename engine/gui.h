#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>

union SDL_Event;

namespace Rml {
class Context;
}

// Game GUI on RmlUi (HTML/CSS-like .rml documents styled by .rcss sheets). RmlUi reads files through kast::readFile
// (disk, or the mounted pak in kast_player), logs to kast::log ("RmlUi: ..."), draws with bgfx and has the bundled
// Roboto as its fallback font face. See README "Game GUI".
namespace kast::gui {

// Starts RmlUi; call after bgfx::init. False (logged) if it fails; GUI calls then do nothing.
bool init();
// Before bgfx::shutdown. Every Context is gone afterwards.
void shutdown();
bool ready();
// Warnings and errors RmlUi has logged since init: a load or parse went wrong if it grew meanwhile.
uint64_t problemCount();

} // namespace kast::gui

namespace kast {

// One RmlUi context (a set of documents sharing a size and input focus), drawn into a bgfx view.
class GuiContext {
public:
    explicit GuiContext(std::string name) : name_(std::move(name)) {}
    ~GuiContext() { release(); }
    GuiContext(const GuiContext&) = delete;
    GuiContext& operator=(const GuiContext&) = delete;

    // Created on first use; null if gui::ready() is false.
    Rml::Context* context();
    void release(); // removes the context and its documents
    // Pixel size of the area the documents lay out in.
    void resize(int width, int height);
    // Updates and draws the documents into view on frameBuffer (BGFX_INVALID_HANDLE = backbuffer) at the resize()
    // size. The view's clear is the caller's.
    void render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer);
    // Mouse, wheel, key and text events. Event positions are mapped to the context as (p - origin) * scale.
    void onEvent(const SDL_Event& event, float originX = 0.0f, float originY = 0.0f, float scale = 1.0f);

private:
    std::string name_;
    Rml::Context* context_ = nullptr;
    int width_ = 1;
    int height_ = 1;
};

} // namespace kast
