#include "jrb/model/Project.hpp"

#include <algorithm>

namespace jrb {

const Track* findTrack(const Project& project, TrackId id) noexcept {
    for (const auto& track : project.tracks)
        if (track->id == id)
            return track.get();
    return nullptr;
}

const Pattern* findPattern(const Project& project, PatternId id) noexcept {
    auto it = std::lower_bound(project.patterns.begin(), project.patterns.end(), id,
                               [](const Shared<Pattern>& p, PatternId key) { return p->id < key; });
    return (it != project.patterns.end() && (*it)->id == id) ? it->get() : nullptr;
}

} // namespace jrb
