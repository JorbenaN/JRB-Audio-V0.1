#include "Check.hpp"

#include "jrb/edit/Commands.hpp"
#include "jrb/edit/Session.hpp"
#include "jrb/model/NoteSequence.hpp"

#include <algorithm>
#include <array>
#include <random>
#include <unordered_set>
#include <vector>

using namespace jrb;

namespace {

bool sameNotes(const NoteSequence& seq, const std::vector<Note>& ref) {
    if (seq.size() != ref.size())
        return false;
    auto it = seq.begin();
    for (const auto& n : ref) {
        const Note& m = *it++;
        if (m.id != n.id || m.pitch != n.pitch || m.start != n.start || m.length != n.length)
            return false;
    }
    return it == seq.end();
}

std::vector<NoteId> overlappingIds(const NoteSequence& seq, Tick a, Tick b) {
    std::vector<NoteId> out;
    seq.forEachOverlapping(a, b, [&](const Note& n) { out.push_back(n.id); });
    return out;
}

std::vector<NoteId> overlappingIdsBrute(const std::vector<Note>& ref, Tick a, Tick b) {
    std::vector<NoteId> out;
    for (const auto& n : ref)
        if (n.start < b && n.start + n.length > a)
            out.push_back(n.id);
    return out;
}

std::vector<Note> makeNotes(std::size_t count, std::uint64_t& nextId) {
    std::vector<Note> notes;
    for (std::size_t i = 0; i < count; ++i)
        notes.push_back({NoteId{nextId++}, 24 + static_cast<int>(i * 5 % 72), static_cast<Tick>(i) * kTicksPerQuarter / 8,
                         kTicksPerQuarter / 8, 0.8f});
    return notes;
}

} // namespace

TEST("NoteSequence: matches a plain sorted vector under 7,500 random edits") {
    for (unsigned seed = 1; seed <= 5; ++seed) {
        std::mt19937 rng(seed);
        auto uniform = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        NoteSequence seq;
        std::vector<Note> ref;
        std::uint64_t nextId = 1;
        std::size_t bytes = 0;
        bool ok = true;

        for (int step = 0; step < 1500 && ok; ++step) {
            const int op = uniform(0, 9);
            std::unordered_set<NoteId> remove;
            std::vector<Note> insert;

            if (op < 5 || ref.empty()) {
                const int count = op == 0 ? uniform(1, 700) : uniform(1, 8);  // occasional large batch
                for (int i = 0; i < count; ++i)
                    insert.push_back({NoteId{nextId++}, uniform(0, 127), uniform(0, 64 * 16) * kTicksPerQuarter / 16,
                                      uniform(1, 64) * kTicksPerQuarter / 16, 0.5f});
            } else {
                const int count = std::min<int>(static_cast<int>(ref.size()), op < 8 ? uniform(1, 20) : uniform(1, 60));
                std::vector<Note> picked;
                std::sample(ref.begin(), ref.end(), std::back_inserter(picked), count, rng);
                for (const auto& n : picked)
                    remove.insert(n.id);
                if (op < 8) {  // move
                    const Tick dt = uniform(-8, 8) * kTicksPerQuarter / 4;
                    const int dp = uniform(-12, 12);
                    for (auto n : picked) {
                        n.start = std::max<Tick>(0, n.start + dt);
                        n.pitch = std::clamp(n.pitch + dp, 0, 127);
                        insert.push_back(n);
                    }
                }
            }

            seq = seq.edited(remove, insert, bytes);
            std::erase_if(ref, [&](const Note& n) { return remove.contains(n.id); });
            ref.insert(ref.end(), insert.begin(), insert.end());
            std::sort(ref.begin(), ref.end(), noteOrder);

            ok = seq.checkInvariants() && sameNotes(seq, ref);
            if (ok && step % 50 == 0) {
                for (int q = 0; q < 10 && ok; ++q) {
                    const Tick a = uniform(0, 70 * 4) * kTicksPerQuarter / 4;
                    const Tick b = a + uniform(1, 32) * kTicksPerQuarter / 4;
                    ok = overlappingIds(seq, a, b) == overlappingIdsBrute(ref, a, b);
                }
            }
        }
        CHECK(ok);
    }
}

TEST("NoteSequence: editing one note in 20,000 shares all but the touched chunks") {
    std::uint64_t nextId = 1;
    std::size_t bytes = 0;
    const NoteSequence base = NoteSequence{}.edited({}, makeNotes(20'000, nextId), bytes);
    CHECK(base.checkInvariants());
    CHECK(base.chunks().size() >= 20'000 / NoteSequence::kMaxChunk);

    Note moved = *base.begin();
    moved.pitch += 1;
    std::size_t editBytes = 0;
    const NoteSequence next = base.edited({moved.id}, {moved}, editBytes);
    CHECK(next.checkInvariants());

    std::size_t shared = 0;
    for (const auto& chunk : next.chunks())
        shared += std::find(base.chunks().begin(), base.chunks().end(), chunk) != base.chunks().end();
    CHECK(shared + 2 >= next.chunks().size());
    CHECK(editBytes < 32 * 1024);  // ~10 KB chunk + pointer vector, not ~800 KB
}

TEST("NoteSequence: deleting most notes merges small chunks back together") {
    std::uint64_t nextId = 1;
    std::size_t bytes = 0;
    NoteSequence seq = NoteSequence{}.edited({}, makeNotes(5'000, nextId), bytes);
    std::unordered_set<NoteId> remove;
    std::size_t i = 0;
    for (const auto& n : seq)
        if (i++ % 50 != 0)
            remove.insert(n.id);  // keep every 50th note
    seq = seq.edited(remove, {}, bytes);
    CHECK(seq.size() == 100);
    CHECK(seq.checkInvariants());
    CHECK(seq.chunks().size() == 1);

    seq = seq.edited({seq.front().id}, {}, bytes);
    CHECK(seq.size() == 99);
    std::unordered_set<NoteId> all;
    for (const auto& n : seq)
        all.insert(n.id);
    seq = seq.edited(all, {}, bytes);
    CHECK(seq.empty() && seq.chunks().empty() && seq.begin() == seq.end());
}

TEST("undo history: byte budget bounds memory and keeps the most recent steps") {
    Session session;
    PatternId big;
    NoteId first;
    (void)session.perform("setup", [&](Draft& d) -> Status {
        big = addPattern(d, "Big", 2560 * kTicksPerQuarter).value();
        std::vector<NoteSpec> specs;
        for (int n = 0; n < 20'000; ++n)
            specs.push_back({24 + (n * 5) % 72, n * kTicksPerQuarter / 8, kTicksPerQuarter / 8, 0.8f});
        first = addNotes(d, big, specs).value().front();
        return ok();
    });

    session.setHistoryByteLimit(1024 * 1024);
    std::vector<ProjectPtr> states{session.snapshot()};
    for (int i = 0; i < 300; ++i) {
        (void)session.perform("Move", [&](Draft& d) { return moveNotes(d, big, std::array{first}, 0, (i % 2) ? -1 : 1); });
        states.push_back(session.snapshot());
    }
    CHECK(session.historyBytes() <= 1024 * 1024);
    CHECK(session.undoDepth() > 20 && session.undoDepth() < 300);

    // Undo walks back through the retained steps exactly.
    const std::size_t depth = session.undoDepth();
    const std::size_t bytesBefore = session.historyBytes();
    for (std::size_t k = 1; k <= depth; ++k) {
        CHECK(session.undo());
        CHECK(session.snapshot() == states[states.size() - 1 - k]);
    }
    CHECK(!session.canUndo());
    CHECK(session.historyBytes() == 0);
    for (std::size_t k = 0; k < depth; ++k)
        CHECK(session.redo());
    CHECK(session.snapshot() == states.back());
    CHECK(session.historyBytes() == bytesBefore);
}
