#include "undo.h"

void UndoStack::push(Step step)
{
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
