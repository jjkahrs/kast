#pragma once

#include <pugixml.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// GUI Builder document model, no UI: an .rml file as an element tree under <body> that the builder edits and saves.
// Panel = <div>, Text = <p>, Button = <button> (data-event = the event its click publishes, see gui_events.h). Any other
// element is Unknown: listed, movable and deletable, never created or edited. Everything the builder doesn't edit
// (other elements, attributes, comments, <head>) is kept; saving normalizes whitespace and indentation only.

enum class GuiKind { Panel, Text, Button, Unknown };
// An RmlUi data binding attribute the GUI Inspector edits (docs/editor/gui-builder.html): data-model, data-for, data-if,
// data-visible, data-value, data-checked, data-rml, data-text, or data-attr-*, data-attrif-*, data-class-*,
// data-style-*, data-event-*, data-alias-* with a name after the prefix. Not kast's own data-event / data-drag.
bool guiBindingAttribute(std::string_view name);
constexpr GuiKind kGuiCreatableKinds[] = {GuiKind::Panel, GuiKind::Text, GuiKind::Button};
const char* guiKindTag(GuiKind kind);   // "div", "p", "button"; "" for Unknown
const char* guiKindLabel(GuiKind kind); // "Panel", "Text", "Button", "Unknown"
GuiKind guiKind(pugi::xml_node node);

class GuiDoc {
public:
    // False + error (document unchanged) if rml is not well-formed or has no <rml><body>.
    bool parse(std::string_view rml, std::string& error);
    std::string serialize() const;
    pugi::xml_node body() const { return xml_.child("rml").child("body"); }

    // Element children of parent, in order (text and comments skipped).
    static std::vector<pugi::xml_node> elements(pugi::xml_node parent);
    // Appends a new element of kind (not Unknown) to parent: Text and Button get placeholder text. Returns it.
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

    // href of the first <link type="text/rcss">; empty if none.
    std::string stylesheetHref() const;
    void setStylesheetHref(std::string_view href);

    // The RML the preview loads: every element under <body> gets data-kast-node="<index into nodes>" (pre-order) and
    // the stylesheet links are replaced by an inline <style> holding rcss (the Style panel's text). nodes gets the
    // elements of this document in that order. Never saved.
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
