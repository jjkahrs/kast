#pragma once

#include "shader_graph_doc.h"
#include "shader_preview.h"

#include <array>
#include <chrono>
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
// - Shader Preview: the graph, unsaved edits included, on a sphere, cube or plane (ShaderPreview); "View generated code"
//   shows the .kshader text the library compiles.
// Live update: 0.3 s after the last edit the graph goes to ShaderLibrary as an override of its file (setOverride), which
// compiles it on its worker while frames keep drawing; every material using the graph (Scene, Game, preview) then
// renders it, the last good version until a compile succeeds. Saving writes the file; opening another graph (or the
// file going away) drops the override, back to the file on disk. Errors and warnings (validation, and shaderc's mapped
// to nodes) outline their node, red or #ffcc00, with the message as its tooltip and in the Graph Inspector; a summary
// line sits on the canvas, and the library logs them.
// Edits mark the graph dirty ("*" in the titles) and are steps on its own undo history, cleared on open and reload; a
// drag, or an Inspector field from activation to release, is one step. Save or Ctrl+S (a graph panel focused) writes
// the file. A file changed on disk reloads when clean, else Reload / Keep mine is asked. Opening another graph or
// quitting while dirty asks Save / Discard / Cancel first.
class ShaderGraphEditor {
public:
    // Window names (stable ids; the visible title adds the file name and dirty marker).
    static constexpr const char* kCanvasWindow = "###Shader Graph";
    static constexpr const char* kInspectorWindow = "###Graph Inspector";
    static constexpr const char* kPreviewWindow = "###Shader Preview";
    static constexpr double kLiveDelay = 0.3; // seconds after the last edit until the graph recompiles

    // The Shader Preview renders on views previewView .. previewView + ShaderPreview::kViews - 1.
    explicit ShaderGraphEditor(bgfx::ViewId previewView) : preview_(previewView) {}

    void shutdown(); // before ImGui's context goes

    // Opens file (after the unsaved-changes prompt if dirty) and brings the panels to the front.
    void open(const std::filesystem::path& file);
    // One panel a frame: Shader Preview, Graph Inspector, then the canvas, which then zooms to fit the graph (at its
    // docked size: framed while still hidden it would fit the wrong size).
    void focusPanels()
    {
        focusStep_ = 3;
        navigateIn_ = 3;
    }
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

    // Live update, once a frame before the views render: the debounced recompile, its result, the node errors. root:
    // the project's (the library's paths are relative to it); a graph outside it isn't previewed.
    void update(const std::filesystem::path& root);
    ShaderPreview& preview() { return preview_; }
    void showGeneratedCode() { codeRequested_ = true; } // the "View generated code" window

    void drawCanvas();
    void drawInspector();
    void drawPreview(float time); // time: TIME for the preview
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
    void applyLive(); // hands the graph to the library if its generated code changed
    void drawIssues(int node) const; // the node's errors and warnings, one line each

    ShaderGraphDoc doc_;
    ShaderPreview preview_;
    // Live update.
    using Clock = std::chrono::steady_clock;
    std::string livePath_;
    unsigned liveRevision_ = 0;
    std::optional<Clock::time_point> editedAt_; // the last edit not yet handed to the library
    std::string liveKey_;                         // what was last handed: its generated code (or JSON, if it has errors)
    std::optional<Clock::time_point> compiling_;  // handed over at the edit at this time, compile not yet in
    int compilingFrames_ = 0;
    std::string liveError_;                       // the library's error the issues were made from
    std::vector<kast::GraphIssue> warnings_;      // the graph's warnings
    std::vector<kast::GraphIssue> issues_;        // warnings_ and the library's errors, by node (0: the graph)
    bool codeRequested_ = false;
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
