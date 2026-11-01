#pragma once

#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Rml {
class Context;
class Element;
class ElementDocument;
} // namespace Rml

// Built-in game GUI tooltips (docs/engine/game-gui.html, "Tooltips"): data-tooltip="text" on any element of a document
// (kast.gui.setTooltip sets it from Lua). The element under the mouse, or its nearest ancestor with a data-tooltip that
// isn't empty or only spaces, gives the text: BBCode (gui_richtext.h, so markup in it never makes elements). After
// kGuiTooltipDelay seconds over it a box shows near the mouse, kGuiTooltipOffsetDp dp right of and below it, flipped to
// the other side of the cursor where it would leave the context's area and kept inside it. It hides at once when the
// mouse leaves the element, a button goes down (a drag needs one held, so it never shows during a drag; after the
// release it stays hidden until the mouse is over another source), or the element is hidden, removed or its document
// unloads or hides. Moving to an element with another tooltip starts the delay again; a changed text on the source
// shows at once.
// The box is <div id="kast-tooltip"> in a document of its own, the context's topmost while it shows, that wears the
// source document's sheets (the default widget style under them, gui::addWidgetStyle): kGuiWidgetStyle's
// #kast-tooltip rule styles it and a project .rcss rule on #kast-tooltip overrides it. GuiContext drives one per
// context (GuiContext::render, onEvent).
namespace kast {

constexpr double kGuiTooltipDelay = 0.5;      // seconds of hover before a tooltip shows
constexpr float kGuiTooltipOffsetDp = 16.0f;  // from the mouse to the box, each axis
constexpr const char* kGuiTooltipAttribute = "data-tooltip";
constexpr const char* kGuiTooltipId = "kast-tooltip";

// Where a box of size (w, h) goes for the mouse at (mouseX, mouseY) in an area of (areaW, areaH), all in one unit:
// offset right of and below the mouse; flipped left / up of it where it would cross the right / bottom edge; then kept
// inside the area (at its left / top edge if it is bigger than the area).
struct GuiTooltipSpot {
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const GuiTooltipSpot&) const = default;
};
GuiTooltipSpot guiTooltipSpot(float mouseX, float mouseY, float w, float h, float areaW, float areaH, float offset);

// The element whose data-tooltip shows over element (it or its nearest ancestor with a non-blank one), null if none.
Rml::Element* guiTooltipSource(Rml::Element* element);

class GuiTooltip {
public:
    // The mouse in context units (GuiContext::onEvent), a button going down and up.
    void mouseMove(float x, float y);
    void press();
    void release();
    // Once a frame after the context's Update: follows the hover, shows, moves, updates or hides the box (laying its
    // document out again when it changed). now: clock() unless given (tests inject time).
    void update(Rml::Context& context);
    void update(Rml::Context& context, double now);
    // The context is gone (GuiContext::release): forgets its document.
    void reset();

    bool showing() const { return shown_; }
    std::string text() const { return shown_ ? text_ : ""; } // the BBCode showing, "" if none
    Rml::Element* box() const { return box_; }               // #kast-tooltip once made, null before
    Rml::Element* source() const { return get(source_); }

    std::function<double()> clock; // seconds; empty: SDL's ticks

private:
    // An element watched until it is destroyed (RmlUi's DLL exports no empty ObserverPtr: an empty optional is none).
    using Watch = std::optional<Rml::ObserverPtr<Rml::Element>>;
    static Rml::Element* get(const Watch& watch) { return watch ? watch->get() : nullptr; }
    static void watch(Watch& watch, Rml::Element* element);

    void show(Rml::Context& context);
    void hide();
    void place(Rml::Context& context);

    Rml::ElementDocument* document_ = nullptr; // made on the first show
    Rml::Element* box_ = nullptr;
    Watch source_;     // what the mouse is over, waiting or showing
    Watch suppressed_; // hidden by a press: not again until another source
    std::string text_;
    double since_ = 0.0; // when the mouse came over source_
    float mouseX_ = 0.0f;
    float mouseY_ = 0.0f;
    bool held_ = false;
    bool shown_ = false;
};

} // namespace kast
