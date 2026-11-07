#pragma once

// CoreAPI: the only C++ surface the Swift app sees (docs/02-architecture.md §6).
//
// Rules for this header:
//  - No JUCE types, no templates beyond std::string / std::vector of plain structs.
//  - Never throws: every entry point catches and reports through lastError().
//  - IDs are plain uint64_t (0 = none). Ticks are int64_t at ticksPerQuarter().
//  - SessionHandle is main-thread only. ProjectView is immutable and may be
//    read from any thread.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_SHARED_REFERENCE(retain, release)
#define SWIFT_RETURNS_RETAINED
#define SWIFT_NAME(name)
#define SWIFT_UNCHECKED_SENDABLE
#endif

namespace jrb {
struct Project;
class Session;
} // namespace jrb

namespace jrb::api {

class SessionHandle;

enum class TrackKind : std::uint8_t { instrument = 0, audio = 1, folder = 2, bus = 3 };

struct NoteInput {
    std::int32_t pitch = 60;
    std::int64_t start = 0;
    std::int64_t length = 0;
    float velocity = 0.8f;
};

struct NoteData {
    std::uint64_t id = 0;
    std::int32_t pitch = 0;
    std::int64_t start = 0;
    std::int64_t length = 0;
    float velocity = 0;
    bool muted = false;
};

struct ClipData {
    std::uint64_t id = 0;
    std::uint64_t patternId = 0;
    std::int64_t start = 0;
    std::int64_t length = 0;
    bool muted = false;
};

struct TrackData {
    std::uint64_t id = 0;
    std::string name;
    TrackKind kind = TrackKind::instrument;
    std::uint8_t colorIndex = 0;
    bool muted = false;
    bool solo = false;
    std::int32_t clipCount = 0;
};

struct PatternData {
    std::uint64_t id = 0;
    std::string name;
    std::int64_t length = 0;
    std::int64_t noteCount = 0;
};

using IdList = std::vector<std::uint64_t>;
using NoteInputList = std::vector<NoteInput>;
using NoteDataList = std::vector<NoteData>;
using ClipDataList = std::vector<ClipData>;

/// An immutable project snapshot. Copying is O(1) (one shared pointer).
/// Sendable: it is never mutated after creation, so any thread may read it.
class ProjectView {
public:
    ProjectView() = default;

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool isEmpty() const noexcept { return project_ == nullptr; }
    /// True when both views refer to the very same snapshot object.
    [[nodiscard]] bool isIdentical(const ProjectView& other) const noexcept { return project_ == other.project_; }

    [[nodiscard]] double tempo() const noexcept;
    [[nodiscard]] std::int32_t trackCount() const noexcept;
    [[nodiscard]] TrackData trackAt(std::int32_t index) const;
    [[nodiscard]] ClipDataList clips(std::uint64_t trackId) const;

    [[nodiscard]] std::int32_t patternCount() const noexcept;
    [[nodiscard]] PatternData patternAt(std::int32_t index) const;
    [[nodiscard]] std::int64_t noteCount(std::uint64_t patternId) const noexcept;
    [[nodiscard]] NoteDataList notes(std::uint64_t patternId) const;
    /// Notes overlapping [start, end) — what an editor needs for the visible region.
    [[nodiscard]] NoteDataList notesInRange(std::uint64_t patternId, std::int64_t start, std::int64_t end) const;

private:
    friend class SessionHandle;
    ProjectView(std::shared_ptr<const Project> project, std::uint64_t revision)
        : project_(std::move(project)), revision_(revision) {}

    std::shared_ptr<const Project> project_;
    std::uint64_t revision_ = 0;
} SWIFT_UNCHECKED_SENDABLE;

[[nodiscard]] std::int64_t ticksPerQuarter() noexcept;

} // namespace jrb::api

void jrb_session_retain(jrb::api::SessionHandle* handle) noexcept;
void jrb_session_release(jrb::api::SessionHandle* handle) noexcept;

namespace jrb::api {

/// A project editing session. Imported into Swift as a reference type.
class SessionHandle {
public:
    [[nodiscard]] static SessionHandle* create() SWIFT_RETURNS_RETAINED;

    SessionHandle(const SessionHandle&) = delete;
    SessionHandle& operator=(const SessionHandle&) = delete;

    [[nodiscard]] ProjectView snapshot() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;

    // Edits. Each returns 0 / false on failure; lastError() then explains why.
    std::uint64_t addTrack(const std::string& name, TrackKind kind);
    bool removeTrack(std::uint64_t trackId);
    bool renameTrack(std::uint64_t trackId, const std::string& name);
    std::uint64_t addPattern(const std::string& name, std::int64_t lengthTicks);
    IdList addNotes(std::uint64_t patternId, const NoteInputList& notes);
    bool moveNotes(std::uint64_t patternId, const IdList& noteIds, std::int64_t deltaTicks, std::int32_t deltaPitch);
    bool deleteNotes(std::uint64_t patternId, const IdList& noteIds);
    std::uint64_t placeClip(std::uint64_t trackId, std::uint64_t patternId, std::int64_t start, std::int64_t length);
    bool setTempo(double bpm);

    // Gestures and history.
    bool beginGesture(const std::string& name);
    bool endGesture();
    bool cancelGesture();
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    bool undo();
    bool redo();
    [[nodiscard]] std::string undoName() const;
    [[nodiscard]] std::string redoName() const;

    [[nodiscard]] std::string lastError() const { return lastError_; }

private:
    SessionHandle();
    ~SessionHandle();
    friend void ::jrb_session_retain(SessionHandle*) noexcept;
    friend void ::jrb_session_release(SessionHandle*) noexcept;

    template <class Fn>
    auto guarded(Fn&& fn, decltype(fn()) fallback) noexcept -> decltype(fn());

    std::unique_ptr<Session> session_;
    std::string lastError_;
    // Atomic because Swift may drop its last reference on any thread.
    std::atomic<std::uint32_t> refCount_{1};
} SWIFT_SHARED_REFERENCE(jrb_session_retain, jrb_session_release);

} // namespace jrb::api
