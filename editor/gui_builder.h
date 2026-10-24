#pragma once

#include "gui_doc.h"
#include "gui_undo.h"
#include "viewport.h"

#include <gui.h>
#include <gui_models.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Rml {
class Element;
class ElementDocument;
} // namespace Rml

// GUI Builder: four panels on one open .rml (gui_doc.h) and the .rcss it links.
// - GUI Hierarchy: element tree; add Panel / Text / Rich Text / Button / Toggle / Slider / Dropdown / Spinbox / Progress / List Box /
//   Virtual List (into the selected Panel, else <body>), delete, move up/down, drag-drop onto a Panel or <body> to
//   reparent; Save. A composite widget (GuiDoc::composite) is one node: its insides are not listed.
// - GUI Inspector: id, class, text (Text, Button) and data-event (Button, the input widgets and the lists) of the
//   selection, its widget properties (GuiDoc::widgetProperties), a Dropdown's options and a List Box's rows (add,
//   remove, move, value, label, selected), a Rich Text's markup (multiline; a toolbar wraps the selection in or puts at
//   the cursor [b] [i] [u] [s] [color] [size] [font] [link] or an [img], each one step; the converter's warnings below
//   it), and on any element its RmlUi data binding attributes (guiBindingAttribute)
//   but those edited as widget properties: edited, emptied to remove, or added by name. Other attributes of Unknown
//   elements are read-only.
// - GUI Style: the .rcss text, applied to the preview 0.3 s after the last keystroke. A sheet RmlUi logs problems for
//   (in the Console) is not applied: the preview keeps the last good one.
// - GUI Preview: the document rendered by RmlUi at a chosen resolution, scaled to fit; click selects the deepest
//   element under the cursor (a composite widget's inside selects the widget); the selection is outlined (an overlay,
//   not in the file). It wears the engine's default widget style under the GUI's own sheet and shows Rich Text
//   converted (with the project's fonts/), as in Play.
// - GUI prefabs (gui_doc.h): a .guiprefab opens like an .rml (its one element under Body, its sibling .rcss in GUI
//   Style). An instance is one node with a prefab marker; its prefab elements are listed and selectable but fixed (no
//   move, delete, reparent or children); a missing prefab's node is red. Create GUI Prefab (node menu, or a node dropped
//   on the Assets panel) and placing (a .guiprefab dropped on a Panel / Body or the Preview) are steps. The Inspector
//   shows a Prefab card (Select Root, Open Prefab; on the root Revert All, Apply All, Unpack) and marks overridden
//   fields #ffcc00 (right-click: Revert, Apply). A prefab saved on disk refreshes the open GUI's instances.
// Edits mark the GUI dirty ("*" in the titles) and are steps on its own undo history (gui_undo.h), cleared when a GUI is
// opened or reloaded; an Inspector or Style text field is one step from activation to letting go. Save or Ctrl+S (a builder panel focused) writes both files. Files
// changed on disk reload when clean, else Reload / Keep mine is asked. Opening another GUI or quitting while dirty asks
// Save / Discard / Cancel first.
class GuiBuilder {
public:
    // Window names (stable ids; the visible title adds the file name and dirty marker).
    static constexpr const char* kHierarchyWindow = "###GUI Hierarchy";
    static constexpr const char* kInspectorWindow = "###GUI Inspector";
    static constexpr const char* kStyleWindow = "###GUI Style";
    static constexpr const char* kPreviewWindow = "###GUI Preview";

    // Draws the preview in previewView .. previewView + kast::gui::kViews - 1.
    explicit GuiBuilder(bgfx::ViewId previewView) : target_(previewView, "GUI Preview") {}
    void shutdown();

    // Opens rml in the builder (after the unsaved-changes prompt if dirty) and brings its panels to the front.
    void open(const std::filesystem::path& rml);
    void focusPanels() { focusStep_ = 4; } // brings the four panels to the front of their docks, one per frame
    // Selects the first element with this id attribute. False if there is none.
    bool selectById(const std::string& id);
    // Selects the deepest element at (x, y) in preview pixels, as a click there does (<body> if none). False if no
    // preview is laid out yet.
    bool pick(float x, float y);
    std::string selectedId() const { return GuiDoc::attribute(GuiDoc::widgetElement(selected_), "id"); } // "" if none
    // Adds an element of kind as the GUI Hierarchy's + buttons do (applied when the panel next draws).
    void requestAdd(GuiKind kind) { edit_ = {Edit::Op::Add, addParent(), {}, kind}; }
    // Deletes, moves (-1 up, +1 down) or reparents (onto the element with that id) the selection as the GUI Hierarchy
    // does (applied when the panel next draws; refused, with a warning, inside a prefab instance).
    void requestDelete() { edit_ = {Edit::Op::Delete, selected_}; }
    void requestMove(int delta) { edit_ = {Edit::Op::Move, selected_, {}, GuiKind::Panel, delta}; }
    bool requestReparent(const std::string& parentId);

    // GUI prefabs (gui_doc.h), each one undo step but Apply, which writes the prefab and clears the history (as a scene
    // prefab's Apply does). False (logged; a refusal shown in a modal) if it can't be done.
    // The Assets panel's current folder (absolute; the project root if none): where Create GUI Prefab writes. Set by the
    // host.
    std::function<std::filesystem::path()> assetFolder;
    // Makes the selection a new .guiprefab in dir and an instance of it.
    bool createPrefab(const std::filesystem::path& dir);
    // Places an instance of the prefab (project path) in the selected Panel (beside a selected instance), else <body>,
    // and selects it.
    bool placePrefab(const std::string& path);
    // Reverts or applies the selection's field (GuiDoc::field names); "*": Revert All / Apply All of its instance.
    bool revertField(const std::string& name);
    bool applyField(const std::string& name);
    // Unpacks the selection's instance.
    bool unpack();
    // The last item (the Assets panel's background or a folder) as a drop target for an element dragged from the GUI
    // Hierarchy: Create GUI Prefab in dir.
    void acceptElementDrop(const std::filesystem::path& dir);
    // Sets (empty: removes) an attribute of the selection as one Inspector edit step, as typing it in does. False if
    // nothing or only <body> is selected.
    bool setAttribute(const std::string& name, const std::string& value);
    // Sets a widget property of the selection (GuiDoc::setWidgetProperty) as one Inspector edit step. False if the
    // selection has no such property.
    bool setWidgetProperty(const std::string& name, const std::string& value);
    // The Rich Text toolbar's edit (guiWrapRichText) of the selection's markup at bytes start..end, as one Inspector
    // step. False if the selection is no Rich Text.
    bool wrapRichText(const std::string& tag, const std::string& value, size_t start, size_t end);
    // The open GUI's undo history (Edit > Undo / Redo while a builder panel is focused).
    const UndoStack& history() const { return history_; }
    // Undo / redo a step, then reselect the element it affected and set the dirty flag from the save point.
    void undo();
    void redo();
    // Records the Inspector or Style text field edit in progress, if any, as one step (main calls it once no widget is
    // active; undo, redo and hierarchy edits call it first).
    void endGesture();
    // Writes the .rml and .rcss (Save button, Ctrl+S). False (logged) if a write fails.
    bool save();
    // Adds WxH to the preview resolutions and picks it.
    void setPreviewSize(int width, int height);
    bool dirty() const { return rmlDirty_ || rcssDirty_; }
    // An Assets rename or move: the open files follow if they were from or inside it.
    void remap(const std::filesystem::path& from, const std::filesystem::path& to);
    // Hot reload check (main's file poll).
    void poll();
    // AppHooks::quitRequested: false while the user still has to answer the unsaved-changes prompt.
    bool onQuitRequested();

    // The panels (the host docks each) and the builder's modals; call once per frame.
    void drawHierarchy();
    void drawInspector();
    void drawStyle();
    void drawPreview();
    void drawModals();

private:
    struct Edit {
        enum class Op { None, Add, Delete, Move, Reparent, PlacePrefab, CreatePrefab } op = Op::None;
        pugi::xml_node node; // Delete, Move, Reparent, CreatePrefab: the element; Add, PlacePrefab: the parent
        pugi::xml_node parent;
        GuiKind kind = GuiKind::Panel;
        int delta = 0;
        std::string path;          // PlacePrefab: the prefab's project path
        std::filesystem::path dir; // CreatePrefab: where
    };
    // A prefab action asked for while the Inspector draws, done once it has (it may replace every node).
    struct PrefabOp {
        enum class Kind { Revert, Apply, Unpack, Open } kind;
        std::string field;
    };

    bool load(const std::filesystem::path& rml); // false (logged, nothing changed) if it can't be read or parsed
    void close();
    bool applyRcss(); // validates rcssText_; on success it becomes the preview's sheet
    void markRmlDirty();
    // Records the step that turned before into after (see pushGuiStep); false if it changed nothing.
    bool record(std::string name, std::optional<GuiElementCopy> before, std::optional<GuiElementCopy> after);
    // One Inspector step on the selection: edit changes it between the before and after copies. False if it changed
    // nothing.
    bool editStep(const std::string& name, const std::function<void()>& edit);
    void drawOptions(); // the selected Dropdown's options or List Box's rows
    void drawRichText(); // the selected Rich Text's markup, toolbar and warnings
    void afterUndoRedo();
    void rememberFileTimes();
    std::string title(const char* name, const char* window) const;
    pugi::xml_node addParent() const; // the selected Panel, else <body>

    void drawNode(pugi::xml_node node);
    void applyEdit();
    std::filesystem::path projectRoot() const; // the open file's project, else its folder
    void prefabFailed(std::string error);      // logged and shown in a modal
    void drawPrefabCard(pugi::xml_node root);  // the Inspector's Prefab card (root null: a missing prefab)
    // The #ffcc00 marker and the Revert / Apply menu after an Inspector field of the selection, if it is overridden.
    void overrideMarker(const std::string& field);
    void runPrefabOp(const PrefabOp& op);
    void reselect(const std::vector<size_t>& path); // after every node was replaced (refresh, Apply)
    void refreshPrefabs(); // a prefab changed on disk while the GUI has unsaved edits: instances re-read, edits kept
    void saveShortcut();
    void rebuildPreview();

    ViewTarget target_;
    kast::GuiModels previewModels_; // empty models for the document's data-model names (before preview_: it points here)
    kast::GuiContext preview_{"gui-preview"};
    Rml::ElementDocument* previewDoc_ = nullptr;
    std::vector<pugi::xml_node> previewNodes_;    // data-kast-node index -> model element
    std::vector<Rml::Element*> previewElements_;  // same index -> preview element
    bool previewStale_ = true;
    std::vector<std::pair<int, int>> resolutions_{{1280, 720}, {1920, 1080}, {0, 0}}; // {0, 0} = the panel's size
    int resolution_ = 0;

    std::filesystem::path rml_; // empty = nothing open
    std::filesystem::path rcss_; // the linked sheet; empty if none
    GuiDoc doc_;
    std::string rcssText_;
    std::string goodRcss_; // the last rcssText_ RmlUi parsed without problems: the preview's sheet
    bool sheetFailed_ = false;
    double rcssEditedAt_ = -1.0; // ImGui time of the last unapplied keystroke; < 0 = none
    bool rmlDirty_ = false;
    bool rcssDirty_ = false;
    std::filesystem::file_time_type rmlTime_, rcssTime_;
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> prefabTimes_; // the prefabs it reads
    pugi::xml_document pristineHolder_;
    pugi::xml_node pristine_; // inspectorNode_ as its prefab has it (GuiDoc::pristine); null outside instances
    std::optional<PrefabOp> prefabOp_;
    std::string prefabError_;
    bool prefabErrorRequested_ = false;
    pugi::xml_node selected_;
    Edit edit_;
    std::string inspectorBuffer_[4]; // id, class, text, data-event of inspectorNode_
    std::map<std::string, std::string> bindingBuffers_; // inspectorNode_'s data binding attributes, by name
    std::string newBinding_;                            // the Inspector's "Add binding" name
    std::vector<std::pair<std::string, std::string>> widgetBuffers_; // inspectorNode_'s widget properties: name, text
    std::vector<std::pair<std::string, std::string>> optionBuffers_; // inspectorNode_'s options / rows: value, label
    pugi::xml_node inspectorNode_;
    std::pair<int, int> richSelection_; // the Rich Text markup field's last selection (bytes; equal: the cursor)
    float richColor_[3] = {1.0f, 0.667f, 0.0f}; // the toolbar's popups' last values
    int richSize_ = 24;
    std::string richFont_ = "Roboto";
    std::string richLink_;
    std::string richImage_;
    int richImageSize_[2] = {0, 0}; // w, h; 0 = auto
    UndoStack history_;
    struct Gesture {
        std::string name;
        pugi::xml_node node;
        GuiElementCopy before;
    };
    std::optional<Gesture> gesture_; // an Inspector field being edited
    std::string rcssCommitted_;      // rcssText_ as of the last Style step (or load)

    std::filesystem::path pendingOpen_; // waits on the unsaved-changes prompt
    bool openPromptRequested_ = false;
    bool reloadPromptRequested_ = false;
    bool quitPromptRequested_ = false;
    bool quitConfirmed_ = false;
    int focusStep_ = 0; // focusPanels: 4 Hierarchy, 3 Inspector, 2 Style, 1 Preview (ImGui focuses one window a frame)
};
