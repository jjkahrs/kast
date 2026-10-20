#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>

// One document's undo history (docs/editor/undo-redo.html): steps already done, each a name ("Move Cube") and the
// actions that undo and redo it. Knows nothing of what it edits: each surface (scene, GUI doc, zone) makes its own
// steps.
class UndoStack {
public:
    static constexpr size_t kMaxSteps = 100;

    struct Step {
        std::string name;
        std::function<void()> undo, redo;
    };

    // Records a step that was just done: the redo branch is discarded and, past kMaxSteps, the oldest step dropped.
    void push(Step step);
    bool canUndo() const { return applied_ > 0; }
    bool canRedo() const { return applied_ < steps_.size(); }
    // The step Undo / Redo would run; empty if none.
    std::string undoName() const { return canUndo() ? steps_[applied_ - 1].name : std::string(); }
    std::string redoName() const { return canRedo() ? steps_[applied_].name : std::string(); }
    // Run the step's action and move past it; no-op if there is none.
    void undo();
    void redo();

    // Drops every step. clean: the document as it is now is the save point (else there is none until markSaved).
    void clear(bool clean);
    // The document as it is now was saved.
    void markSaved() { saved_ = applied_; }
    // An edit that isn't a step changed the document: no undo / redo gets back to the saved state.
    void forgetSavePoint() { saved_.reset(); }
    // Undo / redo is back at the saved state (a discarded or dropped save point never is).
    bool atSavePoint() const { return saved_ == applied_; }
    size_t size() const { return steps_.size(); }

private:
    std::deque<Step> steps_;
    size_t applied_ = 0;              // steps_[0, applied_) are done; the rest is the redo branch
    std::optional<size_t> saved_ = 0; // applied_ at the last save
};

// What Edit > Undo / Redo and Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z act on: the focused panel's document. Each surface wires
// its history and the actions that undo / redo it and then update the document (dirty flag, selection).
struct UndoTarget {
    UndoStack* history = nullptr; // null: nothing to undo
    std::function<void()> undo, redo;
    bool blocked = false; // e.g. during Play: shown greyed, shortcuts ignored
    bool canUndo() const { return history && !blocked && history->canUndo(); }
    bool canRedo() const { return history && !blocked && history->canRedo(); }
};
