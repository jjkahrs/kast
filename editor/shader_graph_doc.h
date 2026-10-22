#pragma once

#include <shader_graph.h>
#include <undo.h>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Shader Graph panels' model (GPU-free, no ImGui): the open .kshadergraph and every canvas and Graph Inspector edit,
// each one step on the graph's own undo history (docs/editor/undo-redo.html). The panels call it; the self-checks test
// it directly. Edits keep the graph one that parseShaderGraph reads back (the strict JSON rules): an edit that would
// break that is refused (false + error, nothing changed). Errors that only generateKShader finds (a missing required
// link, a stage) are allowed: the graph is being built.
class ShaderGraphDoc {
public:
    // --- File ---
    // Opens file (history cleared). False + error (nothing changed) if it can't be read or parsed.
    bool open(const std::filesystem::path& file, std::string& error);
    void close();
    // Writes the graph to its file (shaderGraphText). False + error if the write fails.
    bool save(std::string& error);
    // What poll found: the file changed on disk and was reloaded (clean), or it changed while dirty (Conflict: the
    // caller asks Reload / Keep mine), or it was removed (closed if clean).
    enum class DiskChange { None, Reloaded, Conflict, Removed };
    DiskChange poll();
    // A rename or move of from (a file or folder): the open file follows.
    void remap(const std::filesystem::path& from, const std::filesystem::path& to);
    const std::filesystem::path& file() const { return file_; }
    bool isOpen() const { return !file_.empty(); }
    bool dirty() const { return isOpen() && !history_.atSavePoint(); }
    const kast::ShaderGraph& graph() const { return graph_; }
    const UndoStack& history() const { return history_; }
    void undo();
    void redo();

    // --- Edits: each one undo step, or part of the open gesture ---
    // A node of type at position (settings: values for some of its settings, the rest default). Its id: 0 (nothing
    // added) if the type is unknown or a setting is invalid.
    int addNode(std::string_view type, std::array<float, 2> position, const nlohmann::ordered_json& settings = {},
                std::string* error = nullptr);
    // Links an output to an input (canLink), replacing the input's link. False + why if refused.
    bool link(const kast::GraphPin& from, const kast::GraphPin& to, std::string& error);
    // Removes the link into input to; false if there is none.
    bool unlink(const kast::GraphPin& to);
    // Deletes nodes (and their links), the links into inputs and the comments (indices); one step.
    bool remove(const std::vector<int>& nodes, const std::vector<kast::GraphPin>& links = {}, std::vector<size_t> comments = {});
    // Nodes and comments (their rect's x, y; w, h) where they now are: a drag's step (one per gesture).
    bool move(const std::vector<std::pair<int, std::array<float, 2>>>& nodes,
              const std::vector<std::pair<size_t, std::array<float, 4>>>& comments = {});
    // A node setting (GraphNodeDef::settings): a string or that many numbers. Refused if graphNodeSettingsError
    // objects. A Custom Expression's links and constants on pins it no longer has go with the change.
    bool setSetting(int node, const std::string& name, const nlohmann::ordered_json& value, std::string& error);
    // A constant for an unlinked number input (1 to 4 numbers as the pin takes); empty: back to the pin's default.
    bool setInput(int node, const std::string& pin, const std::vector<float>& values, std::string& error);
    // Params: added with a free name ("param", "param_2", ...) and the type's defaults; edited whole (rename: Param
    // nodes follow; retype: links the new type can't make go); removed (its Param nodes stay, an error until relinked).
    size_t addParam(kast::ShaderParamType type);
    bool setParam(size_t index, const kast::ShaderParam& param, std::string& error);
    bool removeParam(size_t index);
    // Comment frames: added (index returned), retitled or resized (rect) whole.
    size_t addComment(kast::GraphComment comment);
    bool setComment(size_t index, const kast::GraphComment& comment);

    // --- Copy / paste ---
    // The nodes and the links between them (plus the params their Param nodes name), as .kshadergraph text.
    std::string copy(const std::vector<int>& nodes) const;
    // copy's text added as new nodes with fresh ids, their top-left at at; links between them kept, params this graph
    // lacks added. The new ids; empty + error if text holds no graph nodes.
    std::vector<int> paste(std::string_view text, std::array<float, 2> at, std::string& error);

    // --- Gestures: edits from begin to end are one step (a drag, a field from activation to release) ---
    void beginGesture(std::string name);
    void endGesture();
    bool inGesture() const { return gesture_.has_value(); }

private:
    // Records the step from before to the graph as it is now (inside a gesture: nothing yet). False if nothing changed.
    bool commit(std::string name, kast::ShaderGraph before);
    // trial becomes the graph (one step named name) if the strict parse reads it back; else false + error.
    bool apply(std::string name, kast::ShaderGraph trial, std::string& error);
    static kast::GraphNode* find(kast::ShaderGraph& graph, int id);
    // Drops what graph's nodes no longer have pins for (after a settings or param type change): constants on missing or
    // resized inputs, and links canLink now refuses.
    static void dropBroken(kast::ShaderGraph& graph);
    void rememberFileTime();

    std::filesystem::path file_;
    std::filesystem::file_time_type fileTime_{};
    kast::ShaderGraph graph_;
    UndoStack history_;
    struct Gesture {
        std::string name;
        kast::ShaderGraph before;
    };
    std::optional<Gesture> gesture_;
};

// The "New Shader Graph" starter: a Color param into a Fragment Output's ALBEDO.
kast::ShaderGraph starterShaderGraph();
