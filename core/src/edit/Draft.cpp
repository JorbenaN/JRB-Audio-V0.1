#include "jrb/edit/Draft.hpp"

#include <algorithm>

namespace jrb {

Draft::Draft(const Project& base) : project_(base) {
    // The copied entity vectors are new memory owned by the resulting snapshot.
    bytes_ = sizeof(Project) + (project_.tracks.size() + project_.patterns.size()) * sizeof(Shared<Track>);
}

std::size_t Draft::approximateBytes(const Track& track) noexcept {
    return sizeof(Track) + track.name.capacity() + track.clips.capacity() * sizeof(Clip);
}

std::size_t Draft::approximateBytes(const Pattern& pattern) noexcept {
    // Note chunks are shared and accounted separately when an edit rebuilds them.
    return sizeof(Pattern) + pattern.name.capacity() + pattern.notes.containerBytes();
}

template <class T>
T* Draft::own(Shared<T>& slot) {
    if (owned_.contains(slot.get())) {
        // Created by this draft as a non-const object, so the cast is well-defined.
        return const_cast<T*>(slot.get());
    }
    return adopt(std::make_shared<T>(*slot), slot);
}

Track* Draft::editTrack(TrackId id) {
    for (auto& slot : project_.tracks)
        if (slot->id == id)
            return own(slot);
    return nullptr;
}

Pattern* Draft::editPattern(PatternId id) {
    auto& patterns = project_.patterns;
    auto it = std::lower_bound(patterns.begin(), patterns.end(), id,
                               [](const Shared<Pattern>& p, PatternId key) { return p->id < key; });
    if (it == patterns.end() || (*it)->id != id)
        return nullptr;
    return own(*it);
}

} // namespace jrb
