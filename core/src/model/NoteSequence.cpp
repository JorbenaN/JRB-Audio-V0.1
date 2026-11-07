#include "jrb/model/NoteSequence.hpp"

#include <algorithm>
#include <tuple>

namespace jrb {

using ChunkPtr = std::shared_ptr<const NoteChunk>;

bool noteOrder(const Note& a, const Note& b) noexcept {
    return std::tie(a.start, a.pitch, a.id) < std::tie(b.start, b.pitch, b.id);
}

namespace {

ChunkPtr makeChunk(std::vector<Note>::const_iterator first, std::vector<Note>::const_iterator last,
                   std::size_t& bytes) {
    auto chunk = std::make_shared<NoteChunk>();
    chunk->notes.assign(first, last);
    for (const auto& n : chunk->notes)
        chunk->maxEnd = std::max(chunk->maxEnd, n.start + n.length);
    bytes += sizeof(NoteChunk) + chunk->notes.size() * sizeof(Note);
    return chunk;
}

/// Splits sorted notes into chunks of about kTargetChunk notes (never more than kMaxChunk).
std::vector<ChunkPtr> makeChunks(const std::vector<Note>& notes, std::size_t& bytes) {
    std::vector<ChunkPtr> out;
    const std::size_t n = notes.size();
    if (n == 0)
        return out;
    const std::size_t pieces = n <= NoteSequence::kMaxChunk ? 1 : (n + NoteSequence::kTargetChunk - 1) / NoteSequence::kTargetChunk;
    out.reserve(pieces);
    for (std::size_t i = 0; i < pieces; ++i) {
        const auto from = static_cast<std::ptrdiff_t>(n * i / pieces);
        const auto to = static_cast<std::ptrdiff_t>(n * (i + 1) / pieces);
        out.push_back(makeChunk(notes.begin() + from, notes.begin() + to, bytes));
    }
    return out;
}

/// Restores the minimum-size invariant by merging undersized chunks into a neighbour.
void normalize(std::vector<ChunkPtr>& chunks, std::size_t& bytes) {
    std::size_t i = 0;
    while (chunks.size() > 1 && i < chunks.size()) {
        if (chunks[i]->notes.size() >= NoteSequence::kMinChunk) {
            ++i;
            continue;
        }
        // Merge with the next chunk, or with the previous one at the end.
        const std::size_t left = (i + 1 < chunks.size()) ? i : i - 1;
        std::vector<Note> combined;
        combined.reserve(chunks[left]->notes.size() + chunks[left + 1]->notes.size());
        combined.insert(combined.end(), chunks[left]->notes.begin(), chunks[left]->notes.end());
        combined.insert(combined.end(), chunks[left + 1]->notes.begin(), chunks[left + 1]->notes.end());
        auto replacement = makeChunks(combined, bytes);
        auto at = chunks.erase(chunks.begin() + static_cast<std::ptrdiff_t>(left),
                               chunks.begin() + static_cast<std::ptrdiff_t>(left + 2));
        chunks.insert(at, replacement.begin(), replacement.end());
        i = left;  // re-check from the merged position
    }
}

} // namespace

const Note* NoteSequence::find(NoteId id) const noexcept {
    for (const auto& chunk : chunks_)
        for (const auto& note : chunk->notes)
            if (note.id == id)
                return &note;
    return nullptr;
}

NoteSequence NoteSequence::edited(const std::unordered_set<NoteId>& remove, std::vector<Note> insert,
                                  std::size_t& bytesCreated) const {
    std::sort(insert.begin(), insert.end(), noteOrder);

    std::vector<ChunkPtr> out;
    out.reserve(chunks_.size() + 2 + insert.size() / kTargetChunk);
    std::size_t nextInsert = 0;

    for (std::size_t c = 0; c < chunks_.size(); ++c) {
        const NoteChunk& chunk = *chunks_[c];
        const bool lastChunk = c + 1 == chunks_.size();

        // Inserts ordered before this chunk's last note belong here; the last
        // chunk takes everything that remains.
        std::size_t insertEnd = nextInsert;
        if (lastChunk)
            insertEnd = insert.size();
        else
            while (insertEnd < insert.size() && noteOrder(insert[insertEnd], chunk.notes.back()))
                ++insertEnd;

        const bool removesHere = !remove.empty() &&
            std::any_of(chunk.notes.begin(), chunk.notes.end(), [&](const Note& n) { return remove.contains(n.id); });

        if (insertEnd == nextInsert && !removesHere) {
            out.push_back(chunks_[c]);  // untouched: shared
            continue;
        }

        std::vector<Note> kept;
        kept.reserve(chunk.notes.size());
        for (const auto& n : chunk.notes)
            if (!removesHere || !remove.contains(n.id))
                kept.push_back(n);

        std::vector<Note> merged;
        merged.reserve(kept.size() + (insertEnd - nextInsert));
        std::merge(kept.begin(), kept.end(), insert.begin() + static_cast<std::ptrdiff_t>(nextInsert),
                   insert.begin() + static_cast<std::ptrdiff_t>(insertEnd), std::back_inserter(merged), noteOrder);
        nextInsert = insertEnd;

        auto rebuilt = makeChunks(merged, bytesCreated);
        out.insert(out.end(), rebuilt.begin(), rebuilt.end());
    }

    if (chunks_.empty() && !insert.empty())
        out = makeChunks(insert, bytesCreated);

    normalize(out, bytesCreated);

    NoteSequence result;
    result.chunks_ = std::move(out);
    for (const auto& chunk : result.chunks_)
        result.size_ += chunk->notes.size();
    bytesCreated += result.containerBytes();
    return result;
}

bool NoteSequence::checkInvariants() const {
    std::size_t count = 0;
    const Note* previous = nullptr;
    for (const auto& chunk : chunks_) {
        const std::size_t n = chunk->notes.size();
        if (n == 0 || n > kMaxChunk)
            return false;
        if (chunks_.size() > 1 && n < kMinChunk)
            return false;
        Tick maxEnd = 0;
        for (const auto& note : chunk->notes) {
            if (previous && !noteOrder(*previous, note))
                return false;
            previous = &note;
            maxEnd = std::max(maxEnd, note.start + note.length);
        }
        if (maxEnd != chunk->maxEnd)
            return false;
        count += n;
    }
    return count == size_;
}

} // namespace jrb
