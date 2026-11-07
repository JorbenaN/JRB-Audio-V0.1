#include "jrb/api/CoreAPI.hpp"

#include "jrb/edit/Commands.hpp"
#include "jrb/edit/Session.hpp"

#include <algorithm>
#include <exception>

namespace jrb::api {
namespace {

NoteData toData(const Note& n) {
    return {n.id.value, n.pitch, n.start, n.length, n.velocity, n.muted};
}

std::vector<NoteId> toNoteIds(const IdList& ids) {
    std::vector<NoteId> out;
    out.reserve(ids.size());
    for (auto v : ids)
        out.push_back(NoteId{v});
    return out;
}

jrb::TrackKind toModel(TrackKind kind) { return static_cast<jrb::TrackKind>(kind); }

} // namespace

// MARK: ProjectView

double ProjectView::tempo() const noexcept { return project_ ? project_->tempoBpm : 0.0; }

std::int32_t ProjectView::trackCount() const noexcept {
    return project_ ? static_cast<std::int32_t>(project_->tracks.size()) : 0;
}

TrackData ProjectView::trackAt(std::int32_t index) const {
    if (index < 0 || index >= trackCount())
        return {};
    const Track& t = *project_->tracks[static_cast<std::size_t>(index)];
    return {t.id.value, t.name, static_cast<TrackKind>(t.kind), t.colorIndex,
            t.muted, t.solo, static_cast<std::int32_t>(t.clips.size())};
}

ClipDataList ProjectView::clips(std::uint64_t trackId) const {
    ClipDataList out;
    if (!project_)
        return out;
    if (const Track* t = findTrack(*project_, TrackId{trackId})) {
        out.reserve(t->clips.size());
        for (const auto& c : t->clips)
            out.push_back({c.id.value, c.pattern.value, c.start, c.length, c.muted});
    }
    return out;
}

std::int32_t ProjectView::patternCount() const noexcept {
    return project_ ? static_cast<std::int32_t>(project_->patterns.size()) : 0;
}

PatternData ProjectView::patternAt(std::int32_t index) const {
    if (index < 0 || index >= patternCount())
        return {};
    const Pattern& p = *project_->patterns[static_cast<std::size_t>(index)];
    return {p.id.value, p.name, p.length, static_cast<std::int64_t>(p.notes.size())};
}

std::int64_t ProjectView::noteCount(std::uint64_t patternId) const noexcept {
    const Pattern* p = project_ ? findPattern(*project_, PatternId{patternId}) : nullptr;
    return p ? static_cast<std::int64_t>(p->notes.size()) : 0;
}

NoteDataList ProjectView::notes(std::uint64_t patternId) const {
    NoteDataList out;
    if (const Pattern* p = project_ ? findPattern(*project_, PatternId{patternId}) : nullptr) {
        out.reserve(p->notes.size());
        for (const auto& n : p->notes)
            out.push_back(toData(n));
    }
    return out;
}

NoteDataList ProjectView::notesInRange(std::uint64_t patternId, std::int64_t start, std::int64_t end) const {
    NoteDataList out;
    const Pattern* p = project_ ? findPattern(*project_, PatternId{patternId}) : nullptr;
    if (!p)
        return out;
    p->notes.forEachOverlapping(start, end, [&](const Note& n) { out.push_back(toData(n)); });
    return out;
}

std::int64_t ticksPerQuarter() noexcept { return kTicksPerQuarter; }

// MARK: SessionHandle

SessionHandle::SessionHandle() : session_(std::make_unique<Session>()) {}
SessionHandle::~SessionHandle() = default;

SessionHandle* SessionHandle::create() {
    try {
        return new SessionHandle();
    } catch (...) {
        return nullptr;
    }
}

template <class Fn>
auto SessionHandle::guarded(Fn&& fn, decltype(fn()) fallback) noexcept -> decltype(fn()) {
    // Swift cannot catch C++ exceptions; one escaping would terminate the app.
    try {
        lastError_.clear();
        return fn();
    } catch (const std::exception& e) {
        lastError_ = std::string("Internal error: ") + e.what();
    } catch (...) {
        lastError_ = "Internal error.";
    }
    return fallback;
}

ProjectView SessionHandle::snapshot() const { return ProjectView(session_->snapshot(), session_->revision()); }

std::uint64_t SessionHandle::revision() const noexcept { return session_->revision(); }

#define JRB_PERFORM(undoName, call, onSuccess, failure)                            \
    guarded(                                                                       \
        [&]() -> decltype(failure) {                                               \
            auto result = session_->perform(undoName, [&](Draft& d) { return call; }); \
            if (!result) {                                                         \
                lastError_ = result.error().message;                               \
                return failure;                                                    \
            }                                                                      \
            return onSuccess;                                                      \
        },                                                                         \
        failure)

std::uint64_t SessionHandle::addTrack(const std::string& name, TrackKind kind) {
    return JRB_PERFORM("Add Track", jrb::addTrack(d, toModel(kind), name), result.value().value, std::uint64_t{0});
}

bool SessionHandle::removeTrack(std::uint64_t trackId) {
    return JRB_PERFORM("Delete Track", jrb::removeTrack(d, TrackId{trackId}), true, false);
}

bool SessionHandle::renameTrack(std::uint64_t trackId, const std::string& name) {
    return JRB_PERFORM("Rename Track", jrb::renameTrack(d, TrackId{trackId}, name), true, false);
}

std::uint64_t SessionHandle::addPattern(const std::string& name, std::int64_t lengthTicks) {
    return JRB_PERFORM("Add Pattern", jrb::addPattern(d, name, lengthTicks), result.value().value, std::uint64_t{0});
}

IdList SessionHandle::addNotes(std::uint64_t patternId, const NoteInputList& notes) {
    std::vector<NoteSpec> specs;
    specs.reserve(notes.size());
    for (const auto& n : notes)
        specs.push_back({n.pitch, n.start, n.length, n.velocity});
    const char* name = notes.size() == 1 ? "Add Note" : "Add Notes";
    auto toIds = [](const std::vector<NoteId>& ids) {
        IdList out;
        out.reserve(ids.size());
        for (auto id : ids)
            out.push_back(id.value);
        return out;
    };
    return JRB_PERFORM(name, jrb::addNotes(d, PatternId{patternId}, specs), toIds(result.value()), IdList{});
}

bool SessionHandle::moveNotes(std::uint64_t patternId, const IdList& noteIds, std::int64_t deltaTicks,
                              std::int32_t deltaPitch) {
    const auto ids = toNoteIds(noteIds);
    const char* name = ids.size() == 1 ? "Move Note" : "Move Notes";
    return JRB_PERFORM(name, jrb::moveNotes(d, PatternId{patternId}, ids, deltaTicks, deltaPitch), true, false);
}

bool SessionHandle::deleteNotes(std::uint64_t patternId, const IdList& noteIds) {
    const auto ids = toNoteIds(noteIds);
    const char* name = ids.size() == 1 ? "Delete Note" : "Delete Notes";
    return JRB_PERFORM(name, jrb::deleteNotes(d, PatternId{patternId}, ids), true, false);
}

std::uint64_t SessionHandle::placeClip(std::uint64_t trackId, std::uint64_t patternId, std::int64_t start,
                                       std::int64_t length) {
    return JRB_PERFORM("Add Clip", jrb::placeClip(d, TrackId{trackId}, PatternId{patternId}, start, length),
                       result.value().value, std::uint64_t{0});
}

bool SessionHandle::setTempo(double bpm) {
    return JRB_PERFORM("Change Tempo", jrb::setTempo(d, bpm), true, false);
}

#undef JRB_PERFORM

bool SessionHandle::beginGesture(const std::string& name) {
    return guarded([&] { return session_->beginGesture(name); }, false);
}
bool SessionHandle::endGesture() { return session_->endGesture(); }
bool SessionHandle::cancelGesture() { return session_->cancelGesture(); }
bool SessionHandle::canUndo() const noexcept { return session_->canUndo(); }
bool SessionHandle::canRedo() const noexcept { return session_->canRedo(); }
bool SessionHandle::undo() { return session_->undo(); }
bool SessionHandle::redo() { return session_->redo(); }
std::string SessionHandle::undoName() const { return session_->undoName(); }
std::string SessionHandle::redoName() const { return session_->redoName(); }

} // namespace jrb::api

void jrb_session_retain(jrb::api::SessionHandle* handle) noexcept {
    handle->refCount_.fetch_add(1, std::memory_order_relaxed);
}

void jrb_session_release(jrb::api::SessionHandle* handle) noexcept {
    if (handle->refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete handle;
}
