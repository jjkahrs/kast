#include "undo.h"

namespace {
uint64_t undoClock = 0; // ponytail: one clock for every stack, unsynchronized: the editor edits from its main thread only
}

void UndoStack::push(Step step)
{
    pushed_ = step.pushed = ++undoClock;
    steps_.resize(applied_); // the redo branch
    if (saved_ && *saved_ > applied_)
        saved_.reset(); // it was in the discarded branch
    steps_.push_back(std::move(step));
    if (steps_.size() > kMaxSteps) {
        steps_.pop_front();
        if (saved_)
            saved_ = *saved_ == 0 ? std::nullopt : std::optional(*saved_ - 1);
    } else {
        ++applied_;
    }
}

void UndoStack::undo()
{
    if (!canUndo())
        return;
    --applied_;
    steps_[applied_].undone = ++undoClock;
    steps_[applied_].undo();
}

void UndoStack::redo()
{
    if (!canRedo())
        return;
    steps_[applied_].redo();
    ++applied_;
}

void UndoStack::clear(bool clean)
{
    steps_.clear();
    applied_ = 0;
    saved_ = clean ? std::optional<size_t>(0) : std::nullopt;
}

const UndoStack* latestUndo(const UndoStack& a, const UndoStack& b)
{
    if (!a.canUndo() || !b.canUndo())
        return a.canUndo() ? &a : b.canUndo() ? &b : nullptr;
    return a.steps_[a.applied_ - 1].pushed > b.steps_[b.applied_ - 1].pushed ? &a : &b;
}

const UndoStack* nextRedo(const UndoStack& a, const UndoStack& b)
{
    // A redo step stays valid until the other stack records a step after it was undone.
    const auto undone = [](const UndoStack& s, const UndoStack& other) -> uint64_t {
        return s.canRedo() && s.steps_[s.applied_].undone > other.pushed_ ? s.steps_[s.applied_].undone : 0;
    };
    const uint64_t redoA = undone(a, b), redoB = undone(b, a);
    if (!redoA && !redoB)
        return nullptr;
    return redoA > redoB ? &a : &b; // the step undone last
}
