#include "jrb/edit/Commands.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace jrb {
namespace {

constexpr int kTrackColorCount = 12;

Status validate(const NoteSpec& n) {
    if (n.pitch < 0 || n.pitch > 127)
        return fail("Note pitch must be between 0 and 127.");
    if (n.start < 0)
        return fail("Notes cannot start before the beginning of a pattern.");
    if (n.length <= 0)
        return fail("Notes must have a length.");
    if (!std::isfinite(n.velocity) || n.velocity < 0.0f || n.velocity > 1.0f)
        return fail("Note velocity must be between 0 and 1.");
    return ok();
}

std::unordered_set<NoteId> toSet(std::span<const NoteId> ids) { return {ids.begin(), ids.end()}; }

Status requireAllPresent(const Pattern& pattern, const std::unordered_set<NoteId>& ids) {
    std::size_t found = 0;
    for (const auto& note : pattern.notes)
        found += ids.contains(note.id);
    if (found != ids.size())
        return fail("Some of the selected notes no longer exist.");
    return ok();
}

} // namespace

Result<TrackId> addTrack(Draft& draft, TrackKind kind, std::string name) {
    auto track = std::make_shared<Track>();
    track->id = draft.allocateId<TrackTag>();
    track->kind = kind;
    track->name = std::move(name);
    track->colorIndex = static_cast<std::uint8_t>(draft.project().tracks.size() % kTrackColorCount);
    const TrackId id = track->id;
    draft.adopt(std::move(track), draft.root().tracks.emplace_back());
    return id;
}

Status removeTrack(Draft& draft, TrackId id) {
    auto& tracks = draft.root().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Shared<Track>& t) { return t->id == id; });
    if (it == tracks.end())
        return fail("The track no longer exists.");
    // Patterns it referenced are kept: other clips may share them. Unused
    // patterns are cleaned up explicitly by the user, never silently.
    tracks.erase(it);
    return ok();
}

Status renameTrack(Draft& draft, TrackId id, std::string name) {
    Track* track = draft.editTrack(id);
    if (!track)
        return fail("The track no longer exists.");
    track->name = std::move(name);
    return ok();
}

Result<PatternId> addPattern(Draft& draft, std::string name, Tick length) {
    if (length <= 0)
        return fail("A pattern must have a length.");
    auto pattern = std::make_shared<Pattern>();
    pattern->id = draft.allocateId<PatternTag>();
    pattern->name = std::move(name);
    pattern->length = length;
    const PatternId id = pattern->id;
    // IDs grow monotonically, so appending keeps `patterns` sorted by id.
    draft.adopt(std::move(pattern), draft.root().patterns.emplace_back());
    return id;
}

Result<std::vector<NoteId>> addNotes(Draft& draft, PatternId patternId, std::span<const NoteSpec> specs) {
    for (const auto& spec : specs)
        if (auto status = validate(spec); !status)
            return status.error();
    Pattern* pattern = draft.editPattern(patternId);
    if (!pattern)
        return fail("The pattern no longer exists.");

    std::vector<NoteId> ids;
    std::vector<Note> added;
    ids.reserve(specs.size());
    added.reserve(specs.size());
    for (const auto& spec : specs) {
        Note note{draft.allocateId<NoteTag>(), spec.pitch, spec.start, spec.length, spec.velocity};
        ids.push_back(note.id);
        added.push_back(note);
    }
    std::size_t bytes = 0;
    pattern->notes = pattern->notes.edited({}, std::move(added), bytes);
    draft.account(bytes);
    return ids;
}

Status moveNotes(Draft& draft, PatternId patternId, std::span<const NoteId> noteIds, Tick deltaTicks, int deltaPitch) {
    const Pattern* current = findPattern(draft.project(), patternId);
    if (!current)
        return fail("The pattern no longer exists.");
    const auto ids = toSet(noteIds);
    if (auto status = requireAllPresent(*current, ids); !status)
        return status;
    std::vector<Note> moved;
    moved.reserve(ids.size());
    for (const auto& note : current->notes) {
        if (!ids.contains(note.id))
            continue;
        if (note.pitch + deltaPitch < 0 || note.pitch + deltaPitch > 127)
            return fail("Notes cannot be moved outside the keyboard range.");
        if (note.start + deltaTicks < 0)
            return fail("Notes cannot be moved before the beginning of the pattern.");
        Note copy = note;
        copy.start += deltaTicks;
        copy.pitch += deltaPitch;
        moved.push_back(copy);
    }

    Pattern* pattern = draft.editPattern(patternId);
    std::size_t bytes = 0;
    pattern->notes = pattern->notes.edited(ids, std::move(moved), bytes);
    draft.account(bytes);
    return ok();
}

Status deleteNotes(Draft& draft, PatternId patternId, std::span<const NoteId> noteIds) {
    const Pattern* current = findPattern(draft.project(), patternId);
    if (!current)
        return fail("The pattern no longer exists.");
    const auto ids = toSet(noteIds);
    if (auto status = requireAllPresent(*current, ids); !status)
        return status;
    Pattern* pattern = draft.editPattern(patternId);
    std::size_t bytes = 0;
    pattern->notes = pattern->notes.edited(ids, {}, bytes);
    draft.account(bytes);
    return ok();
}

Result<ClipId> placeClip(Draft& draft, TrackId trackId, PatternId patternId, Tick start, Tick length) {
    if (start < 0)
        return fail("Clips cannot start before the beginning of the song.");
    if (length <= 0)
        return fail("A clip must have a length.");
    if (!findPattern(draft.project(), patternId))
        return fail("The pattern no longer exists.");
    const Track* existing = findTrack(draft.project(), trackId);
    if (!existing)
        return fail("The track no longer exists.");
    if (existing->kind != TrackKind::instrument)
        return fail("Patterns can only be placed on instrument tracks.");

    Track* track = draft.editTrack(trackId);
    Clip clip{draft.allocateId<ClipTag>(), start, length, patternId};
    auto pos = std::upper_bound(track->clips.begin(), track->clips.end(), clip, [](const Clip& a, const Clip& b) {
        return a.start != b.start ? a.start < b.start : a.id < b.id;
    });
    track->clips.insert(pos, clip);
    return clip.id;
}

Status setTempo(Draft& draft, double bpm) {
    if (!std::isfinite(bpm) || bpm < kMinTempo || bpm > kMaxTempo)
        return fail("Tempo must be between 10 and 999 BPM.");
    draft.root().tempoBpm = bpm;
    return ok();
}

} // namespace jrb
