#pragma once

// The single writer of the project model (docs/02-architecture.md §5).
// Not thread-safe: call from the main thread only. Snapshots it hands out are
// immutable and may be read from any thread.

#include "jrb/edit/Draft.hpp"
#include "jrb/edit/Result.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jrb {

class Session {
public:
    Session();
    explicit Session(ProjectPtr initial);

    [[nodiscard]] const ProjectPtr& snapshot() const noexcept { return current_; }

    /// Incremented every time the published snapshot changes (edit, undo, redo).
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    /// Applies `edit` to a draft of the current snapshot. On success the draft
    /// becomes the new snapshot and one undo step named `name` is recorded
    /// (or merged into the open gesture). On failure nothing changes.
    template <class Edit>
    auto perform(std::string_view name, Edit&& edit) {
        Draft draft(*current_);
        auto result = std::forward<Edit>(edit)(draft);
        if (result)
            commit(name, std::move(draft));
        return result;
    }

    /// A gesture groups every edit between begin and end into one undo step,
    /// e.g. all intermediate positions of a note drag.
    bool beginGesture(std::string name);
    bool endGesture();
    bool cancelGesture();
    [[nodiscard]] bool inGesture() const noexcept { return gesture_.has_value(); }

    [[nodiscard]] bool canUndo() const noexcept { return !gesture_ && !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !gesture_ && !redo_.empty(); }
    bool undo();
    bool redo();
    [[nodiscard]] std::string undoName() const { return canUndo() ? undo_.back().name : std::string(); }
    [[nodiscard]] std::string redoName() const { return canRedo() ? redo_.back().name : std::string(); }

    /// History is trimmed (oldest first) when it exceeds either limit. The most
    /// recent step is always kept, even if it alone exceeds the byte budget.
    void setHistoryLimit(std::size_t steps);
    void setHistoryByteLimit(std::size_t bytes);
    [[nodiscard]] std::size_t undoDepth() const noexcept { return undo_.size(); }
    /// Approximate memory kept alive by the undo history.
    [[nodiscard]] std::size_t historyBytes() const noexcept { return undoBytes_; }

private:
    struct Step {
        ProjectPtr state;
        std::string name;
        std::size_t bytes = 0;  // approximate memory this step keeps alive
    };

    void commit(std::string_view name, Draft&& draft);
    void publish(ProjectPtr next);
    void pushUndo(Step step);
    void trimHistory();

    ProjectPtr current_;
    std::deque<Step> undo_;
    std::vector<Step> redo_;
    std::optional<Step> gesture_;  // state before the gesture started
    bool gestureChanged_ = false;
    std::uint64_t revision_ = 0;
    std::size_t historyLimit_ = 500;
    std::size_t historyByteLimit_ = 256u * 1024u * 1024u;  // docs/05 §3
    std::size_t undoBytes_ = 0;
};

} // namespace jrb
