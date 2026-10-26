#pragma once

#include "gui_scale.h"

#include <RmlUi/Core/Types.h>
#include <bgfx/bgfx.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

union SDL_Event;
struct SDL_Window;

namespace Rml {
class Context;
class Element;
class ElementDocument;
}

// Game GUI on RmlUi (HTML/CSS-like .rml documents styled by .rcss sheets). RmlUi reads files through kast::readFile
// (disk, or the mounted pak in kast_player), logs to kast::log ("RmlUi: ..."), draws with bgfx and has the bundled
// Roboto (regular, the fallback face, plus bold, italic and bold italic) and the project's fonts/. See
// docs/engine/game-gui.html.
namespace kast::gui {

// bgfx views one GuiContext::render takes: its view and the kViews - 1 after it. A document draws in the first until it
// pushes a layer (filter, backdrop-filter, box-shadow); each later switch of render target (layer, composite, saved
// texture, filter pass) takes the next view, in order. Hosts reserve the whole range for each context they render in a
// frame. A blurred box-shadow costs about 9 views the frame its texture is made (then it is cached), a blur filter
// 3 to 5 per frame, a color filter 2.
constexpr bgfx::ViewId kViews = 65; // the editor fits two of these with two viewports (main.cpp) in bgfx's 256 views

// Starts RmlUi; call after bgfx::init. False (logged) if it fails; GUI calls then do nothing.
bool init();
// Before bgfx::shutdown. Every Context is gone afterwards.
void shutdown();
bool ready();
// True once a document had to read the backbuffer a GuiContext::render drew on (backdrop-filter reads what is under
// it; that frame drew it without the effect). A host drawing the GUI on the backbuffer should from then on draw the
// scene and GUI into a sampleable framebuffer and put it on screen with present().
bool wantsReadableTarget();
// Copies texture over the whole backbuffer (width x height pixels) in view.
void present(bgfx::ViewId view, bgfx::TextureHandle texture, int width, int height);
// Warnings and errors RmlUi has logged since init: a load or parse went wrong if it grew meanwhile.
uint64_t problemCount();
// SDL sends typed text (SDL_EVENT_TEXT_INPUT, what RmlUi text fields insert) only while text input is on: turns it on
// for window while want (a text field has focus, SceneGui::focusedField), off otherwise.
void syncTextInput(SDL_Window* window, bool want);
// A file as RmlUi should see it: UTF-8, '/', and a drive's "C:" as "C|" (RmlUi's own convention: its URLs would read
// "C:" as a scheme and lose the folder an <img src> is relative to). fromRmlPath undoes it for opening the file.
std::string toRmlPath(const std::filesystem::path& file);
std::filesystem::path fromRmlPath(std::string_view path);
// While an element is dragged: its document's source URL, so the drag clone RmlUi builds outside any document still
// finds the images its paths name relative to that document. Empty when no drag runs (gui_events.h sets it).
void setDragDocument(std::string sourceUrl);
// Puts the engine's default form widget style (gui_widgets.h) under document's own sheets, which override it. Call once
// a document has loaded (SceneGui, the GUI Builder preview); elements added to it later are styled too.
void addWidgetStyle(Rml::ElementDocument& document);
// True while a GuiContext::render updates its documents (data binding writes its views then): a widget change seen
// then is the game's doing, not the player's.
bool updating();
// Loads every <projectRoot>/fonts/**.ttf / .otf (through kast::readFile: from the pak in kast_player) not loaded yet,
// under the family its file names, its bold / italic faces told apart by their weight and style. A file that fails is
// logged with its name (RmlUi logs why too) and not tried again; the others still load. Faces stay until shutdown.
// Call before a document loads (SceneGui, the GUI Builder preview).
void loadProjectFonts(const std::filesystem::path& projectRoot);
// True if a face of this font family (any case) is loaded: the bundled Roboto or a project font.
bool fontFamilyLoaded(std::string_view family);
// Every <p class="rich"> under root holding only text gets that text, trimmed, as rich text (guiRichTextRml, a [font=]
// of an unloaded family shown as text). Call once a document has loaded (SceneGui, the GUI Builder preview).
void convertRichText(Rml::Element& root);

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
    // Pixel size of the area the documents draw in, and how they follow the dp ratio (gui_scale.h). Dp: they lay out at
    // width x height, a dp being dpRatio pixels. All: they lay out at width / dpRatio x height / dpRatio (dp == px) and
    // draw scaled up by dpRatio, so px scale too.
    void resize(int width, int height, float dpRatio = 1.0f, GuiScaling scaling = GuiScaling::Dp);
    // Updates and draws the documents into view on frameBuffer (BGFX_INVALID_HANDLE = backbuffer) at the resize()
    // size, using views view .. view + gui::kViews - 1 (only the first unless a document has effects). The view's
    // clear is the caller's. shareViews: drawn over another context rendered just before in the same range, in the
    // views after its (view is ignored). clear (with shareViews): frameBuffer is cleared to
    // transparent first.
    void render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer, bool shareViews = false, bool clear = false);
    // Mouse, wheel, key and text events. Event positions are mapped to the area's pixels as (p - origin) * scale (then
    // to the context's units).
    void onEvent(const SDL_Event& event, float originX = 0.0f, float originY = 0.0f, float scale = 1.0f);
    bool created() const { return context_ != nullptr; }
    // RmlUi units per area pixel: 1 / dpRatio for All, else 1.
    float unitsPerPixel() const { return scaling_ == GuiScaling::All ? 1.0f / dpRatio_ : 1.0f; }

private:
    Rml::Vector2i layoutSize() const;

    std::string name_;
    Rml::Context* context_ = nullptr;
    int width_ = 1;
    int height_ = 1;
    float dpRatio_ = 1.0f;
    GuiScaling scaling_ = GuiScaling::Dp;
};

} // namespace kast
