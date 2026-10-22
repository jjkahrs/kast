#pragma once

#include "shader_graph_doc.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ax::NodeEditor {
struct EditorContext;
} // namespace ax::NodeEditor

// Shader Graph: two panels on one open .kshadergraph (ShaderGraphDoc), in the GUI Builder's pattern (gui_builder.h).
// - Shader Graph: the canvas (imgui-node-editor). Right-drag pans, the wheel zooms; nodes have typed, colored pins.
//   Drag nodes, or a box around them (Ctrl / Shift add to the selection); drag pin to pin to link (a link canLink
//   refuses shows red with the reason). Right-click or Space opens the add menu (searchable, by category) at the
//   cursor; dragging a pin into empty space opens it with only the nodes that can link to that pin, and links the one
//   added. Delete, Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D (pasted at the cursor, fresh ids, links between them kept).
//   Comment frames: titled, resized from their edges; dragging one moves the nodes inside it.
// - Graph Inspector: the selected node's settings and constant inputs, or a selected frame's title; with nothing
//   selected, the graph's params (add, remove, rename, retype, defaults).
// Edits mark the graph dirty ("*" in the titles) and are steps on its own undo history, cleared on open and reload; a
// drag, or an Inspector field from activation to release, is one step. Save or Ctrl+S (a graph panel focused) writes
// the file. A file changed on disk reloads when clean, else Reload / Keep mine is asked. Opening another graph or
// quitting while dirty asks Save / Discard / Cancel first.
class ShaderGraphEditor {
public:
    // Window names (stable ids; the visible title adds the file name and dirty marker).
    static constexpr const char* kCanvasWindow = "###Shader Graph";
    static constexpr const char* kInspectorWindow = "###Graph Inspector";

    void shutdown(); // before ImGui's context goes

    // Opens file (after the unsaved-changes prompt if dirty) and brings the panels to the front.
    void open(const std::filesystem::path& file);
    void focusPanels() { focusStep_ = 2; } // one panel a frame: Graph Inspector, then the canvas
    // The open graph's model: the --graph-* flags edit it as the panels do.
    ShaderGraphDoc& doc() { return doc_; }
    const UndoStack& history() const { return doc_.history(); }
    void undo();
    void redo();
    // Records the Inspector field or drag in progress as one step (main calls it once no widget is active).
    void endGesture() { doc_.endGesture(); }
    bool save(); // Save button, Ctrl+S; false (logged) if the write fails
    bool dirty() const { return doc_.dirty(); }
    // An Assets rename or move: the open file follows.
    void remap(const std::filesystem::path& from, const std::filesystem::path& to) { doc_.remap(from, to); }
    void poll(); // hot reload check (main's file poll)
    bool onQuitRequested(); // AppHooks::quitRequested: false while the unsaved-changes prompt waits

    // The canvas selection (node ids); select applies when the canvas next draws.
    const std::vector<int>& selection() const { return selection_; }
    void select(std::vector<int> nodes) { pendingSelect_ = std::move(nodes); }
    // Opens the add menu at the canvas' center; from a pin: only nodes that can link to it, linked on add.
    void openAddMenu(std::optional<kast::GraphPin> from);
    // Shows a refused link (red, with why) between two pins for a few seconds, as a refused drag does.
    void showRefusedLink(const kast::GraphPin& from, const kast::GraphPin& to, std::string why);
    void frameAll() { navigateIn_ = 2; } // the canvas zooms to fit the whole graph
    // The canvas position at the center of its panel (where --graph-add and the CLI-opened add menu put nodes).
    std::array<float, 2> viewCenter() const { return viewCenter_; }

    void drawCanvas();
    void drawInspector();
    void drawModals();

private:
    struct AddEntry {
        std::string label;    // "Multiply", "Param: tint"
        std::string category;
        std::string type;
        nlohmann::ordered_json settings; // a Param node's "param"
        std::string pin;      // with a pin to link from: the new node's pin that takes (or feeds) it
    };
    struct Refused {
        kast::GraphPin from, to;
        std::string why;
        double until = 0.0;
    };

    bool load(const std::filesystem::path& file);
    std::string title(const char* name, const char* window) const;
    void saveShortcut();
    void drawNodes();
    void drawComments();
    void syncFromCanvas(); // positions, sizes and the selection the canvas changed, into the model
    void handleCreate();
    void handleDelete();
    void handleShortcuts();
    void requestAddMenu(std::optional<kast::GraphPin> from, bool fromInput, std::array<float, 2> at);
    void drawAddMenu();
    std::vector<AddEntry> addEntries() const;
    void drawNodeInspector(const kast::GraphNode& node);
    void drawParams();
    // A text field committed when it lets go; refused text stays in it with the error under it.
    std::optional<std::string> textField(const char* label, const std::string& key, const std::string& value,
                                         bool multiline = false);
    void showFieldError(const std::string& key, bool ok, const std::string& error);

    ShaderGraphDoc doc_;
    ax::NodeEditor::EditorContext* canvas_ = nullptr;
    bool resetCanvas_ = false; // a graph was opened: a fresh canvas (pan, zoom, node ids)
    int navigateIn_ = 0;       // frames until the canvas frames the whole graph
    std::vector<int> selection_;
    std::vector<size_t> selectedComments_;
    std::optional<std::vector<int>> pendingSelect_;
    std::array<float, 2> viewCenter_{};
    std::array<float, 2> mouse_{}; // the cursor on the canvas, this frame

    bool addMenuRequested_ = false;
    bool addMenuAtCenter_ = false;  // opened by the CLI: placed at the panel's center, not the cursor
    std::optional<kast::GraphPin> addFrom_;
    bool addFromInput_ = false; // addFrom_ is an input: the added node feeds it
    std::array<float, 2> addAt_{};
    std::vector<AddEntry> addEntries_; // built when the menu opens
    std::string addSearch_;
    std::optional<Refused> refused_;

    std::map<std::string, std::string> buffers_; // text fields' text while edited, by key
    std::string editingKey_;
    std::map<std::string, std::pair<std::string, std::string>> fieldErrors_; // key -> refused text, why

    std::filesystem::path pendingOpen_;
    bool openPromptRequested_ = false;
    bool reloadPromptRequested_ = false;
    bool quitPromptRequested_ = false;
    bool quitConfirmed_ = false;
    int focusStep_ = 0;
};
