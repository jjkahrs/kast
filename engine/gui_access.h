#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// What Lua systems may do to the running scene's game GUIs (kast.gui.*, docs/engine/game-gui.html). SceneGui implements it;
// LuaSystems only sees this, so it needs no RmlUi. Elements are found by id in the loaded documents, topmost first.
namespace kast {

// A data model value (kast.gui.set / get): null (unset), a string, number or boolean, an array or a table of string keys,
// nested. Plain JSON, so it holds no RmlUi or Lua type.
using GuiValue = nlohmann::json;

// A name RmlUi accepts for a model's top-level variable: a letter, then letters, digits and '_'; not one of its reserved
// names (it, it_index, ev, true, false, size, literal; any case).
inline bool guiVariableName(std::string_view name)
{
    static constexpr std::string_view kReserved[] = {"it", "it_index", "ev", "true", "false", "size", "literal"};
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front())) || !std::ranges::all_of(name, word))
        return false;
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return std::ranges::find(kReserved, lower) == std::end(kReserved);
}

class GuiAccess {
public:
    virtual ~GuiAccess() = default;
    // The value of the form widget, list or text field with this id (gui_widgets.h, gui_lists.h): a bool (toggle, any
    // checkbox), a number (slider, spinbox, progress), a string (dropdown, text field), a list's selected row's value
    // or key (a string) or index (a number), null for a list without a selection; nullopt if no loaded document has one.
    virtual std::optional<GuiValue> value(const std::string& id) = 0;
    // Sets a widget's or text field's value as the player could (snapped and clamped; a dropdown's must be one of its
    // options' values) or selects a list's row (a string: by value or key, a number: by index, null: none); a value of
    // the wrong type, no such row, or an id that is no widget, logs a warning (never the value) and changes nothing.
    // Publishes no change event. Kept like setText, unless no loaded widget took it.
    virtual void setValue(const std::string& id, GuiValue value) = 0;
    // Scrolls the List Box's or Virtual List's 0-based row index into view (scrollToEnd: to its last row) in the
    // topmost document holding the id; no such list or row logs a warning and does nothing. Not kept across reloads.
    virtual void scrollTo(const std::string& id, int index) = 0;
    virtual void scrollToEnd(const std::string& id) = 0;
    // A text field's value, else the element's text (escaped; "\n" is a line break). Kept and applied again to every
    // document that loads later (Play start, hot reload) until reset().
    virtual void setText(const std::string& id, std::string_view text) = 0;
    // The element's content as rich text: BBCode (gui_richtext.h) turned into safe RML; a text field takes it as typed.
    // Kept like setText; the later of setText and setRichText on an id wins.
    virtual void setRichText(const std::string& id, std::string_view text) = 0;
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
    // Replaces the top-level variable key of the data model named model (data-model="model" in an .rml) and refreshes
    // the views bound to it. Kept across document reloads until reset(); a document that loads later shows it.
    virtual void setModelValue(const std::string& model, const std::string& key, GuiValue value) = 0;
    // model.key as it is now, two-way bound fields' edits included; nullopt if never set nor bound.
    virtual std::optional<GuiValue> modelValue(const std::string& model, const std::string& key) = 0;
    // Live elements, gone when their document reloads (never kept like setText). False (logged) if an id is missing or
    // newId is taken in that document:
    // a deep copy of templateId (from parentId's document) appended to parentId as newId, shown (its display property
    // removed); descendant ids starting with "<templateId>_" are renamed to start with "<newId>_".
    virtual bool cloneElement(const std::string& templateId, const std::string& parentId, const std::string& newId) = 0;
    // A new instance of the GUI prefab at the project path prefab (gui_prefabs.h: nested prefabs expanded, ids inside it
    // prefixed "<newId>_", its .rcss added to the document if not yet) appended to parentId as newId. False (logged) if
    // the prefab is missing or broken, no loaded document has parentId, or one of its ids is taken in that document.
    virtual bool instantiatePrefab(const std::string& prefab, const std::string& parentId, const std::string& newId) = 0;
    // A new <tag id="newId"> with attributes appended to parentId.
    virtual bool createElement(const std::string& parentId, const std::string& tag, const std::string& newId,
                               const std::map<std::string, std::string>& attributes) = 0;
    // Removes the element with this id from the topmost document holding one.
    virtual bool removeElement(const std::string& id) = 0;
    // A new scene's systems are about to start: forget the kept writes and model values and unload the documents.
    virtual void reset() = 0;
};

} // namespace kast
