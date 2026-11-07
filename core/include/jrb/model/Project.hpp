#pragma once

// Immutable project model. See docs/05-data-model-and-project-format.md.
//
// A ProjectPtr is a snapshot: it is never mutated after publication, so any
// thread may read it without locks. Edits produce a new snapshot that shares
// every unchanged entity with the previous one (see edit/Draft.hpp).

#include "jrb/model/Ids.hpp"
#include "jrb/model/NoteSequence.hpp"
#include "jrb/model/Time.hpp"

#include <memory>
#include <string>
#include <vector>

namespace jrb {

template <class T>
using Shared = std::shared_ptr<const T>;

struct Pattern {
    PatternId id;
    std::string name;
    Tick length = 4 * kTicksPerQuarter;
    NoteSequence notes;  // sorted by (start, pitch, id), chunked for cheap edits
};

enum class TrackKind : std::uint8_t { instrument, audio, folder, bus };

struct Clip {
    ClipId id;
    Tick start = 0;
    Tick length = 0;
    PatternId pattern;
    Tick sourceOffset = 0;
    bool muted = false;
};

/// Clips are kept sorted by (start, id).
struct Track {
    TrackId id;
    TrackKind kind = TrackKind::instrument;
    std::string name;
    std::uint8_t colorIndex = 0;
    bool muted = false;
    bool solo = false;
    std::vector<Clip> clips;
};

struct Project {
    double tempoBpm = 120.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;

    std::vector<Shared<Track>> tracks;      // user order
    std::vector<Shared<Pattern>> patterns;  // sorted by id (append-only allocation keeps it sorted)

    std::uint64_t nextId = 1;               // next entity id to allocate
};

using ProjectPtr = Shared<Project>;

[[nodiscard]] const Track* findTrack(const Project& project, TrackId id) noexcept;
[[nodiscard]] const Pattern* findPattern(const Project& project, PatternId id) noexcept;

} // namespace jrb
