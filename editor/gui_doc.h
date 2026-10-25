#pragma once

#include <gui_prefabs.h>

#include <pugixml.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GUI Builder document model, no UI: an .rml file as an element tree under <body> that the builder edits and saves.
// Panel = <div>, Text = <p>, Rich Text = <p class="rich"> (its text BBCode, gui_richtext.h), Button = <button> (data-event = the event its click publishes, see gui_events.h), and the
// form widgets by their markup (gui_widgets.h; data-event = the event their changes publish): Toggle = <input
// type="checkbox" class="toggle"/>, alone or as the only element in a <label> holding its text; Slider = <input
// type="range"/>; Dropdown = <select>; Spinbox = <div class="spinbox"> holding exactly <button>, <input>, <button>;
// Progress = <progress>; and the lists (gui_lists.h): List Box = <div class="listbox"> (its rows: its children with class
// "row"); Virtual List = <div class="virtuallist"> holding exactly one element, its row template, which has a data-for.
// A labelled Toggle, a Dropdown, a Spinbox and the lists are composites: one element in the tree, their insides edited
// as widget properties, options and rows. Any other element is Unknown: listed, movable and deletable, never created or
// edited. Everything the builder doesn't edit (other elements, attributes, comments, <head>) is kept;
// saving normalizes whitespace and indentation only.

enum class GuiKind { Panel, Text, Button, Toggle, Slider, Dropdown, Spinbox, Progress, ListBox, VirtualList, RichText, Unknown };
// An RmlUi data binding attribute the GUI Inspector edits (docs/editor/gui-builder.html): data-model, data-for, data-if,
// data-visible, data-value, data-checked, data-rml, data-text, or data-attr-*, data-attrif-*, data-class-*,
// data-style-*, data-event-*, data-alias-* with a name after the prefix. Not kast's own data-event / data-drag.
bool guiBindingAttribute(std::string_view name);
constexpr int kGuiPreviewRows = 20; // placeholder rows a Virtual List shows in the GUI Builder preview
constexpr GuiKind kGuiCreatableKinds[] = {GuiKind::Panel,    GuiKind::Text,     GuiKind::RichText, GuiKind::Button,
                                           GuiKind::Toggle,   GuiKind::Slider,   GuiKind::Dropdown, GuiKind::Spinbox,
                                           GuiKind::Progress, GuiKind::ListBox,  GuiKind::VirtualList};
// "Panel", "Text", "Button", "Toggle", ..., "List Box", "Virtual List", "Rich Text", "Unknown"
const char* guiKindLabel(GuiKind kind);
GuiKind guiKind(pugi::xml_node node);

// GUI prefabs (gui_prefabs.h, docs/editor/gui-builder.html "GUI prefabs"): once setPrefabContext names the project, parse
// expands every instance in place with the engine's expander (the tree holds what Play shows; the instance root keeps
// data-prefab) and serialize collapses each back to its reference, its overrides worked out by comparing it with a fresh
// expansion: a value set back to the prefab's is no override. Prefab files are read once and kept until refreshPrefabs.
// Inside an instance only attributes and text change: add, remove, move and reparent refuse an element inside one (its
// root itself moves like any element) and adding into one. A reference that doesn't expand stays as it was (overrides
// kept, saved unchanged), marked kGuiMissingPrefabAttribute.
class GuiDoc {
public:
    // Where prefab paths resolve (the project root, else the GUI's folder) and the document's own path there (in warnings;
    // a .guiprefab can't hold itself). Unset: instances are kept as written, not expanded.
    void setPrefabContext(const std::filesystem::path& root, std::string documentPath);
    // False + error (document unchanged) if rml is not well-formed or has no <rml><body>.
    bool parse(std::string_view rml, std::string& error);
    // A .guiprefab: exactly one element (else false + error), edited as the only element under body().
    bool parsePrefab(std::string_view text, std::string& error);
    bool isPrefab() const { return prefab_; }
    // The .rml, or for a prefab its one element; instances collapsed to references.
    std::string serialize() const;
    pugi::xml_node body() const { return xml_.child("rml").child("body"); }

    // Re-reads every prefab: instances are collapsed against the prefabs as last read, then expanded again from the files.
    // Every node handle goes stale (element paths stay: no structure outside instances changes).
    void refreshPrefabs();
    // The prefab files and prefab .rcss the instances read (project paths), and the .rcss in the order a document links them.
    const std::vector<std::string>& prefabFiles() const { return prefabFiles_; }
    const std::vector<std::string>& prefabSheets() const { return prefabSheets_; }
    // The outermost expanded instance root holding node (node itself included); null if none.
    static pugi::xml_node instanceRoot(pugi::xml_node node);
    // Inside an instance but not its root: only its attributes and text can change.
    static bool prefabLocked(pugi::xml_node node);
    // A reference whose prefab didn't expand (missing, broken, a cycle).
    static bool missingPrefab(pugi::xml_node node);
    // node can take new children: not inside or the root of an instance, no missing placeholder, and in a prefab not
    // <body> once it holds the prefab's element.
    bool canAddInto(pugi::xml_node node) const;
    // node's element in a fresh expansion of its instance (the prefab's values, no overrides of this instance), kept in
    // holder; null if node is in no instance.
    pugi::xml_node pristine(pugi::xml_node node, pugi::xml_document& holder) const;
    // A field as the Inspector edits it: "text" (text()), a widget property (widgetProperties), else an attribute of
    // widgetElement(node); nullopt for an attribute it doesn't have.
    static std::optional<std::string> field(pugi::xml_node node, std::string_view name);
    static void setField(pugi::xml_node node, std::string_view name, const std::optional<std::string>& value);
    // node (in an instance) has this field differing from the prefab's. Never the instance root's id.
    bool overridden(pugi::xml_node node, std::string_view name) const;
    // Sets an instance root's id, renaming the ids inside it that start with "<old id>_". An empty id is refused.
    static bool setInstanceId(pugi::xml_node root, std::string_view id);

    // Each returns false + error (document unchanged) or does it; the caller records the undo step.
    // Appends a new instance of the prefab at the project path to parent (canAddInto), its id the prefab's file stem made
    // unique in the document (_2, _3, ...). Refused if the prefab doesn't expand (missing, broken) or would hold the
    // document's own prefab (a cycle).
    pugi::xml_node placePrefab(const std::string& path, pugi::xml_node parent, std::string& error);
    // Writes node (not <body>, nothing inside an instance nor an instance root) as dir/<its id, else its kind>.guiprefab
    // plus an .rcss holding only a comment (_2, _3, ... if either name is taken; never overwrites), ids inside it that
    // start with "<its id>_" saved without that (so the instance's ids stay as they were), and replaces node with an
    // instance of it (its id: node's, else the file stem). Refused if the prefab would hold itself. file: the new
    // .guiprefab. Returns the instance.
    pugi::xml_node createPrefab(pugi::xml_node node, const std::filesystem::path& dir, std::filesystem::path& file,
                                std::string& error);
    // A field (field()) of node in an instance back to the prefab's value. False if not overridden.
    bool revert(pugi::xml_node node, std::string_view name);
    // The whole instance (root) back to the prefab: returns its new root.
    pugi::xml_node revertAll(pugi::xml_node root);
    // The instance root becomes plain elements, ids as expanded; instances nested in it stay linked (what it changed on
    // them now their own overrides).
    static bool unpack(pugi::xml_node root);
    // Writes node's field (empty name: every override of root's whole instance) into the outermost prefab's file, then
    // refreshPrefabs. False + error if the file can't be read or written.
    bool apply(pugi::xml_node node, std::string_view name, std::string& error);
    // Rewrites data-prefab paths from (or inside) from to where remapProjectPaths puts them. True if any changed.
    bool remapPrefabPaths(const std::string& from, const std::string& to);

    // Element children of parent, in order (text and comments skipped).
    static std::vector<pugi::xml_node> elements(pugi::xml_node parent);
    // Appends a new element of kind (not Unknown) to parent: Text and Button get placeholder text, Rich Text the markup
    // "[b]Rich[/b] text", a Toggle a <label>
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
    // Text, Rich Text and Buttons whose children are only text (or none) have editable text (Rich Text's: its BBCode,
    // saved verbatim).
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
    kast::GuiPrefabExpansion expansion(bool quiet) const; // reads through prefabCache_
    void expandInstances();
    // The prefab path's reference, collapsed from the instance root (overrides by comparing with a fresh expansion).
    void collapse(pugi::xml_node root) const;
    void collapseUnder(pugi::xml_node node) const; // every instance under node (not node)

    pugi::xml_document xml_;
    bool prefab_ = false;
    std::filesystem::path prefabRoot_; // empty: instances not expanded
    std::string documentPath_;
    mutable std::map<std::string, std::optional<std::string>> prefabCache_; // project path -> text, as first read
    std::vector<std::string> prefabFiles_, prefabSheets_;
};

// The .rcss beside a .guiprefab (<stem>.rcss); its Style panel and Save use it.
std::filesystem::path guiPrefabSheet(const std::filesystem::path& prefab);
// Renames a .guiprefab to newName (renameAsset rules), its sibling <stem>.rcss with it if there is one. False + error
// (disk unchanged) if a name is invalid or taken or a rename fails.
bool renameGuiPrefab(const std::filesystem::path& prefab, const std::string& newName, std::string& error);
// Moves a .guiprefab into destDir (moveAsset rules), its sibling .rcss with it. False + error (disk unchanged).
bool moveGuiPrefab(const std::filesystem::path& prefab, const std::filesystem::path& destDir, std::string& error);
// The Rich Text toolbar's edit of markup (byte offsets start..end: the selection, or the cursor when equal; clamped and
// ordered): [tag] or [tag=value] put before it and [/tag] after it, or for "img" [img=value] put at start (an image
// has no closing tag).
std::string guiWrapRichText(std::string_view markup, size_t start, size_t end, std::string_view tag, std::string_view value);

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
