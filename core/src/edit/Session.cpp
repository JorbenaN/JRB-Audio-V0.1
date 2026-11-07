#include "jrb/edit/Session.hpp"

#include <algorithm>

namespace jrb {

Session::Session() : Session(std::make_shared<const Project>()) {}

Session::Session(ProjectPtr initial) : current_(std::move(initial)) {}

void Session::commit(std::string_view name, Draft&& draft) {
    const std::size_t bytes = draft.bytesWritten();
    auto next = std::make_shared<const Project>(std::move(draft).release());
    if (gesture_) {
        // Intermediate gesture states are discarded; the step's cost is about
        // the largest single edit (a drag keeps rewriting the same entities).
        gestureChanged_ = true;
        gesture_->bytes = std::max(gesture_->bytes, bytes);
    } else {
        pushUndo({current_, std::string(name), bytes});
        redo_.clear();
    }
    publish(std::move(next));
}

void Session::publish(ProjectPtr next) {
    current_ = std::move(next);
    ++revision_;
}

void Session::pushUndo(Step step) {
    undoBytes_ += step.bytes;
    undo_.push_back(std::move(step));
    trimHistory();
}

bool Session::beginGesture(std::string name) {
    if (gesture_)
        return false;
    gesture_ = Step{current_, std::move(name), 0};
    gestureChanged_ = false;
    return true;
}

bool Session::endGesture() {
    if (!gesture_)
        return false;
    if (gestureChanged_) {
        pushUndo(std::move(*gesture_));
        redo_.clear();
    }
    gesture_.reset();
    return true;
}

bool Session::cancelGesture() {
    if (!gesture_)
        return false;
    if (gestureChanged_)
        publish(std::move(gesture_->state));
    gesture_.reset();
    return true;
}

bool Session::undo() {
    if (!canUndo())
        return false;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    undoBytes_ -= step.bytes;
    redo_.push_back({current_, step.name, step.bytes});
    publish(std::move(step.state));
    return true;
}

bool Session::redo() {
    if (!canRedo())
        return false;
    Step step = std::move(redo_.back());
    redo_.pop_back();
    ProjectPtr target = std::move(step.state);
    step.state = current_;
    pushUndo(std::move(step));
    publish(std::move(target));
    return true;
}

void Session::setHistoryLimit(std::size_t steps) {
    historyLimit_ = steps;
    trimHistory();
}

void Session::setHistoryByteLimit(std::size_t bytes) {
    historyByteLimit_ = bytes;
    trimHistory();
}

void Session::trimHistory() {
    while (undo_.size() > historyLimit_ || (undo_.size() > 1 && undoBytes_ > historyByteLimit_)) {
        undoBytes_ -= undo_.front().bytes;
        undo_.pop_front();
    }
}

} // namespace jrb
