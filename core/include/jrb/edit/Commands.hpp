#pragma once

// Edit operations. Each one validates everything before changing anything, so
// a failed command leaves the draft untouched. Commands are run through
// Session::perform, which turns a successful draft into a new snapshot.

#include "jrb/edit/Draft.hpp"
#include "jrb/edit/Result.hpp"

#include <span>
#include <string>
#include <vector>

namespace jrb {

struct NoteSpec {
    int pitch = 60;
    Tick start = 0;
    Tick length = kTicksPerQuarter / 4;
    float velocity = 0.8f;
};

inline constexpr double kMinTempo = 10.0;
inline constexpr double kMaxTempo = 999.0;

Result<TrackId> addTrack(Draft& draft, TrackKind kind, std::string name);
Status removeTrack(Draft& draft, TrackId track);
Status renameTrack(Draft& draft, TrackId track, std::string name);

Result<PatternId> addPattern(Draft& draft, std::string name, Tick length);

Result<std::vector<NoteId>> addNotes(Draft& draft, PatternId pattern, std::span<const NoteSpec> notes);
Status moveNotes(Draft& draft, PatternId pattern, std::span<const NoteId> notes, Tick deltaTicks, int deltaPitch);
Status deleteNotes(Draft& draft, PatternId pattern, std::span<const NoteId> notes);

Result<ClipId> placeClip(Draft& draft, TrackId track, PatternId pattern, Tick start, Tick length);

Status setTempo(Draft& draft, double bpm);

} // namespace jrb
