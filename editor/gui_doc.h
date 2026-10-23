#pragma once

#include <pugixml.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// GUI Builder document model, no UI: an .rml file as an element tree under <body> that the builder edits and saves.
// Panel = <div>, Text = <p>, Button = <button> (data-event = the event its click publishes, see gui_events.h), and the
// form widgets by their markup (gui_widgets.h; data-event = the event their changes publish): Toggle = <input
// type="checkbox" class="toggle"/>, alone or as the only element in a <label> holding its text; Slider = <input
// type="range"/>; Dropdown = <select>; Spinbox = <div class="spinbox"> holding exactly <button>, <input>, <button>;
// Progress = <progress>; and the lists (gui_lists.h): List Box = <div class="listbox"> (its rows: its children with class
// "row"); Virtual List = <div class="virtuallist"> holding exactly one element, its row template, which has a data-for.
// A labelled Toggle, a Dropdown, a Spinbox and the lists are composites: one element in the tree, their insides edited
// as widget properties, options and rows. Any other element is Unknown: listed, movable and deletable, never created or
// edited. Everything the builder doesn't edit (other elements, attributes, comments, <head>) is kept;
// saving normalizes whitespace and indentation only.

enum class GuiKind { Panel, Text, Button, Toggle, Slider, Dropdown, Spinbox, Progress, ListBox, VirtualList, Unknown };
// An RmlUi data binding attribute the GUI Inspector edits (docs/editor/gui-builder.html): data-model, data-for, data-if,
// data-visible, data-value, data-checked, data-rml, data-text, or data-attr-*, data-attrif-*, data-class-*,
// data-style-*, data-event-*, data-alias-* with a name after the prefix. Not kast's own data-event / data-drag.
bool guiBindingAttribute(std::string_view name);
constexpr int kGuiPreviewRows = 20; // placeholder rows a Virtual List shows in the GUI Builder preview
constexpr GuiKind kGuiCreatableKinds[] = {GuiKind::Panel,    GuiKind::Text,    GuiKind::Button,   GuiKind::Toggle,
                                           GuiKind::Slider,   GuiKind::Dropdown, GuiKind::Spinbox, GuiKind::Progress,
                                           GuiKind::ListBox,  GuiKind::VirtualList};
const char* guiKindLabel(GuiKind kind); // "Panel", "Text", "Button", "Toggle", ..., "List Box", "Virtual List", "Unknown"
GuiKind guiKind(pugi::xml_node node);

class GuiDoc {
public:
    // False + error (document unchanged) if rml is not well-formed or has no <rml><body>.
    bool parse(std::string_view rml, std::string& error);
    std::string serialize() const;
    pugi::xml_node body() const { return xml_.child("rml").child("body"); }

    // Element children of parent, in order (text and comments skipped).
    static std::vector<pugi::xml_node> elements(pugi::xml_node parent);
    // Appends a new element of kind (not Unknown) to parent: Text and Button get placeholder text, a Toggle a <label>
    // ("Toggle"), a Slider 0..100 step 1 value 50, a Dropdown three options (the first selected), a Spinbox 0..10 step
    // 1 value 0, a Progress value 0.5 max 1, a List Box three rows (row1..row3, none selected), a Virtual List model
    // "list", row height 24 and the row template <div class="row" data-for="item : items">{{item.name}}</div>.
    // Returns it.
    pugi::xml_node add(GuiKind kind, pugi::xml_node parent);
    // Removes node and its subtree; false for <body> or a node outside it.
    bool remove(pugi::xml_node node);
    // Swaps node with its previous (-1) or next (+1) element sibling; false at either end.
    bool move(pugi::xml_node node, int delta);
    // Moves node to the end of parent's children; false if parent is node or inside it, or node is <body>.
    bool reparent(pugi::xml_node node, pugi::xml_node parent);

    static std::string attribute(pugi::xml_node node, const char* name);
    static void setAttribute(pugi::xml_node node, const char* name, std::string_view value); // empty removes it
    // Text and Buttons whose children are only text (or none) have editable text.
    static bool hasEditableText(pugi::xml_node node);
    static std::string text(pugi::xml_node node);
    static void setText(pugi::xml_node node, std::string_view text);

    // A labelled Toggle, a Dropdown, a Spinbox or a list: its children are the widget's insides, not tree nodes.
    static bool composite(pugi::xml_node node);
    // node, or the outermost composite holding it (what a click on an inside selects).
    static pugi::xml_node widgetRoot(pugi::xml_node node);
    // The element holding node's id, class, data-event and bindings: a labelled Toggle's <input>, else node.
    static pugi::xml_node widgetElement(pugi::xml_node node);
    // The widget properties the Inspector edits on node, by name: Slider min, max, step, value; Spinbox min, max, step
    // (its data-min, data-max, data-step), value and data-value (its <input>'s); Progress value, max; Toggle checked
    // ("true" / "false") and, if labelled, label; List Box follow (data-follow, "true" / "false"); Virtual List model,
    // key, row-height, follow (its data-model, data-key, data-row-height, data-follow), source (the array its row
    // template's data-for names, the alias kept) and, if the row template holds only text, template (that text,
    // {{item.name}} and all). Empty for other kinds.
    static std::vector<std::string> widgetProperties(pugi::xml_node node);
    static std::string widgetProperty(pugi::xml_node node, std::string_view name); // "" if node has no such property
    // True if this attribute of widgetElement(node) is edited as one of its widget properties, or is the <input> type
    // that makes it a Slider or Toggle (the Inspector doesn't list it with the other attributes).
    static bool widgetAttribute(pugi::xml_node node, std::string_view attribute);
    // Empty removes an attribute property. False if node has no such property.
    static bool setWidgetProperty(pugi::xml_node node, std::string_view name, std::string_view value);
    // A Dropdown's <option> elements, in order. An option's value is its value attribute, its label its text.
    static std::vector<pugi::xml_node> options(pugi::xml_node select);
    // Appends <option value="optionN">Option N</option> (N = the option count after it) to select. Returns it.
    static pugi::xml_node addOption(pugi::xml_node select);
    // Makes option the selected one (selected="" on it, removed from the others).
    static void selectOption(pugi::xml_node select, pugi::xml_node option);
    // A List Box's rows (its element children with class "row"), in order. A row's value is its data-value, its label
    // its text; the selected one has class "selected".
    static std::vector<pugi::xml_node> rows(pugi::xml_node listbox);
    // Appends <div class="row" data-value="rowN">Row N</div> (N = the row count after it) to listbox. Returns it.
    static pugi::xml_node addRow(pugi::xml_node listbox);
    // Makes row the selected one (class "selected" on it, off the others); a null row selects none.
    static void selectRow(pugi::xml_node listbox, pugi::xml_node row);
    static bool rowSelected(pugi::xml_node row);

    // href of the first <link type="text/rcss">; empty if none.
    std::string stylesheetHref() const;
    void setStylesheetHref(std::string_view href);

    // The RML the preview loads: every element under <body> gets data-kast-node="<index into nodes>" (pre-order) and
    // the stylesheet links are replaced by an inline <style> holding rcss (the Style panel's text). A Virtual List shows
    // kGuiPreviewRows placeholder rows made from its template: its bindings dropped, each {{a.b}} read as "b <n>". nodes
    // gets the elements of this document in that order. Never saved.
    std::string previewRml(std::string_view rcss, std::vector<pugi::xml_node>& nodes) const;

private:
    pugi::xml_document xml_;
};

// Writes a template GUI as dir/new_gui[_N].rml + .rcss (a Panel holding a Text and a Button; the .rml links the .rcss):
// the first N for which neither name is taken in dir (ignoring case). Sets rml to the .rml. False + error if a write
// fails. Never overwrites a file.
bool writeNewGui(const std::filesystem::path& dir, std::filesystem::path& rml, std::string& error);
// Renames an .rml to newName (renameAsset rules). If it links its sibling <old stem>.rcss, that file is renamed to
// <new stem>.rcss and the link rewritten. False + error (disk unchanged) if a name is invalid or taken or a rename
// fails.
bool renameGui(const std::filesystem::path& rml, const std::string& newName, std::string& error);
// The .rcss file the .rml at rml links (resolved next to it); empty if it links none, can't be parsed, or the link
// leaves the .rml's project (its folder if it is in no project): Save writes the sheet.
std::filesystem::path linkedStylesheet(const std::filesystem::path& rml);
