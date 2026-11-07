#pragma once

// A Draft is a private, mutable working copy of a snapshot used while an edit
// is being applied. Copying the base is shallow (vectors of shared pointers);
// an entity is deep-copied only when an edit asks to modify it (copy-on-write),
// and at most once per draft. Everything untouched stays shared with the base.

#include "jrb/model/Project.hpp"

#include <unordered_set>

namespace jrb {

class Draft {
public:
    explicit Draft(const Project& base);

    Draft(const Draft&) = delete;
    Draft& operator=(const Draft&) = delete;
    Draft(Draft&&) = default;
    Draft& operator=(Draft&&) = default;

    [[nodiscard]] const Project& project() const noexcept { return project_; }

    /// Scalar fields and the entity vectors themselves (insert/remove/reorder).
    /// Never modify an entity reached through this; use editTrack/editPattern.
    [[nodiscard]] Project& root() noexcept { return project_; }

    template <class Tag>
    [[nodiscard]] Id<Tag> allocateId() noexcept { return Id<Tag>{project_.nextId++}; }

    /// Returns a mutable copy of the entity owned by this draft, or nullptr.
    [[nodiscard]] Track* editTrack(TrackId id);
    [[nodiscard]] Pattern* editPattern(PatternId id);

    /// Registers an entity created by this draft as already owned.
    template <class T>
    T* adopt(std::shared_ptr<T> fresh, Shared<T>& slot) {
        T* raw = fresh.get();
        bytes_ += approximateBytes(*raw);
        slot = std::move(fresh);
        owned_.insert(raw);
        return raw;
    }

    /// Records memory allocated by an edit outside adopt(), e.g. note chunks.
    void account(std::size_t bytes) noexcept { bytes_ += bytes; }

    /// Approximate memory this draft allocated that is not shared with its base.
    /// Used to budget undo history (an undo step keeps about this much alive).
    [[nodiscard]] std::size_t bytesWritten() const noexcept { return bytes_; }

    [[nodiscard]] Project release() && { return std::move(project_); }

private:
    template <class T>
    T* own(Shared<T>& slot);

    static std::size_t approximateBytes(const Track& track) noexcept;
    static std::size_t approximateBytes(const Pattern& pattern) noexcept;

    Project project_;
    std::unordered_set<const void*> owned_;
    std::size_t bytes_ = 0;
};

} // namespace jrb
