#pragma once

// The notes of a pattern, stored as a sorted list of immutable, shared chunks
// (ADR-022). An edit rebuilds only the chunks it touches; every other chunk is
// shared with the previous snapshot. Editing one note in a 20,000-note
// pattern therefore copies ~10 KB instead of ~800 KB, which keeps undo history
// small and edits fast.
//
// Invariants (checked by checkInvariants()):
//  - notes are globally sorted by noteOrder (start, pitch, id) across chunks;
//  - no chunk is empty, and none holds more than kMaxChunk notes;
//  - when there is more than one chunk, every chunk holds at least kMinChunk notes;
//  - each chunk's maxEnd equals the largest start + length within it.

#include "jrb/model/Ids.hpp"
#include "jrb/model/Time.hpp"

#include <cstddef>
#include <iterator>
#include <memory>
#include <unordered_set>
#include <vector>

namespace jrb {

struct Note {
    NoteId id;
    int pitch = 60;          // 0...127
    Tick start = 0;          // relative to pattern start
    Tick length = kTicksPerQuarter / 4;
    float velocity = 0.8f;   // 0...1
    bool muted = false;
};

/// Orders notes the way they are stored.
[[nodiscard]] bool noteOrder(const Note& a, const Note& b) noexcept;

struct NoteChunk {
    std::vector<Note> notes;  // non-empty, sorted
    Tick maxEnd = 0;          // max(start + length) over notes
};

class NoteSequence {
public:
    static constexpr std::size_t kTargetChunk = 256;
    static constexpr std::size_t kMaxChunk = 512;
    static constexpr std::size_t kMinChunk = 64;

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Note;
        using difference_type = std::ptrdiff_t;
        using pointer = const Note*;
        using reference = const Note&;

        const_iterator() = default;
        reference operator*() const { return (*chunks_)[chunk_]->notes[index_]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() {
            if (++index_ == (*chunks_)[chunk_]->notes.size()) {
                ++chunk_;
                index_ = 0;
            }
            return *this;
        }
        const_iterator operator++(int) {
            auto copy = *this;
            ++*this;
            return copy;
        }
        friend bool operator==(const const_iterator& a, const const_iterator& b) noexcept {
            return a.chunk_ == b.chunk_ && a.index_ == b.index_;
        }

    private:
        friend class NoteSequence;
        const_iterator(const std::vector<std::shared_ptr<const NoteChunk>>* chunks, std::size_t chunk)
            : chunks_(chunks), chunk_(chunk) {}

        const std::vector<std::shared_ptr<const NoteChunk>>* chunks_ = nullptr;
        std::size_t chunk_ = 0;
        std::size_t index_ = 0;
    };

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const_iterator begin() const noexcept { return {&chunks_, 0}; }
    [[nodiscard]] const_iterator end() const noexcept { return {&chunks_, chunks_.size()}; }
    [[nodiscard]] const Note& front() const { return chunks_.front()->notes.front(); }
    [[nodiscard]] const Note& back() const { return chunks_.back()->notes.back(); }
    [[nodiscard]] const std::vector<std::shared_ptr<const NoteChunk>>& chunks() const noexcept { return chunks_; }

    /// Linear search. Returns nullptr if absent.
    [[nodiscard]] const Note* find(NoteId id) const noexcept;

    /// Calls fn(note) for every note sounding anywhere in [start, end), in order.
    /// Skips whole chunks that end before `start`.
    template <class Fn>
    void forEachOverlapping(Tick start, Tick end, Fn&& fn) const {
        if (end <= start)
            return;
        for (const auto& chunk : chunks_) {
            if (chunk->notes.front().start >= end)
                break;
            if (chunk->maxEnd <= start)
                continue;
            for (const auto& note : chunk->notes) {
                if (note.start >= end)
                    break;
                if (note.start + note.length > start)
                    fn(note);
            }
        }
    }

    /// Returns a new sequence with every note whose id is in `remove` taken out
    /// and the notes in `insert` added. Untouched chunks are shared with *this.
    /// Ids in `remove` that do not exist are ignored (callers validate first).
    /// `bytesCreated` is increased by the memory newly allocated for the result.
    [[nodiscard]] NoteSequence edited(const std::unordered_set<NoteId>& remove, std::vector<Note> insert,
                                      std::size_t& bytesCreated) const;

    /// Approximate bytes of the sequence's own container (not the shared chunks).
    [[nodiscard]] std::size_t containerBytes() const noexcept {
        return chunks_.size() * sizeof(std::shared_ptr<const NoteChunk>);
    }

    [[nodiscard]] bool checkInvariants() const;

private:
    std::vector<std::shared_ptr<const NoteChunk>> chunks_;
    std::size_t size_ = 0;
};

} // namespace jrb
