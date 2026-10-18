#pragma once

#include <optional>
#include <string>
#include <string_view>

// What Lua systems may do to the running scene's game GUIs (kast.gui.*, docs/engine/game-gui.html). SceneGui implements it;
// LuaSystems only sees this, so it needs no RmlUi. Elements are found by id in the loaded documents, topmost first.
namespace kast {

class GuiAccess {
public:
    virtual ~GuiAccess() = default;
    // The value of the text field (<input>, <textarea>) with this id; nullopt if no loaded document has one.
    virtual std::optional<std::string> value(const std::string& id) = 0;
    // A text field's value, else the element's text (escaped; "\n" is a line break). Kept and applied again to every
    // document that loads later (Play start, hot reload) until reset().
    virtual void setText(const std::string& id, std::string_view text) = 0;
    // Hides (display: none) or shows (the style sheet's display) the element. Kept like setText.
    virtual void setVisible(const std::string& id, bool visible) = 0;
    // Sets an attribute of the element (src of an <img>, class, ...). Kept like setText.
    virtual void setAttribute(const std::string& id, const std::string& name, std::string_view value) = 0;
    // Gives keyboard focus to the element (a text field: typing goes there). False if no visible document has it.
    virtual bool focus(const std::string& id) = 0;
    // The id of the text field that has keyboard focus; nullopt if none does.
    virtual std::optional<std::string> focusedField() = 0;
    // True while the mouse is over an element of a visible document other than the document itself (a full-screen
    // <body> doesn't count), e.g. so a press on the chat box doesn't also turn the camera.
    virtual bool mouseOver() = 0;
    // The id of the element under the mouse, or of its nearest ancestor with one (e.g. a bag slot under its icon);
    // nullopt over no element or only a document's <body>. For tooltips.
    virtual std::optional<std::string> hovered() = 0;
    // A new scene's systems are about to start: forget the kept writes and unload the documents.
    virtual void reset() = 0;
};

} // namespace kast
