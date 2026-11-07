#include "Check.hpp"

#include "jrb/api/CoreAPI.hpp"
#include "jrb/edit/Commands.hpp"
#include "jrb/edit/Session.hpp"

#include <algorithm>
#include <array>

using namespace jrb;

namespace {

constexpr Tick kSixteenth = kTicksPerQuarter / 4;

struct Fixture {
    Session session;
    TrackId drums, synth;
    PatternId beat, melody;

    Fixture() {
        auto r = session.perform("setup", [&](Draft& d) -> Status {
            drums = addTrack(d, TrackKind::instrument, "Drums").value();
            synth = addTrack(d, TrackKind::instrument, "Synth").value();
            beat = addPattern(d, "Beat", 4 * kTicksPerQuarter).value();
            melody = addPattern(d, "Melody", 4 * kTicksPerQuarter).value();
            return ok();
        });
        (void)r;
    }

    std::vector<NoteId> add(PatternId p, std::vector<NoteSpec> specs) {
        auto r = session.perform("Add Notes", [&](Draft& d) { return addNotes(d, p, specs); });
        return r ? r.value() : std::vector<NoteId>{};
    }

    const Pattern& pattern(PatternId id) const { return *findPattern(*session.snapshot(), id); }
};

bool sortedNotes(const Pattern& p) { return std::is_sorted(p.notes.begin(), p.notes.end(), noteOrder); }

} // namespace

TEST("snapshots are immutable: an edit never changes an older snapshot") {
    Fixture f;
    const ProjectPtr before = f.session.snapshot();
    f.add(f.melody, {{60, 0, kSixteenth, 0.8f}});
    CHECK(findPattern(*before, f.melody)->notes.empty());
    CHECK(f.pattern(f.melody).notes.size() == 1);
}

TEST("structural sharing: untouched entities are shared, touched ones are copied once") {
    Fixture f;
    const ProjectPtr before = f.session.snapshot();
    (void)f.session.perform("two edits", [&](Draft& d) -> Status {
        (void)addNotes(d, f.melody, std::array{NoteSpec{60, 0, kSixteenth, 0.8f}});
        (void)addNotes(d, f.melody, std::array{NoteSpec{62, kSixteenth, kSixteenth, 0.8f}});
        return ok();
    });
    const ProjectPtr after = f.session.snapshot();
    CHECK(before->patterns[0] == after->patterns[0]);   // Beat: shared
    CHECK(before->patterns[1] != after->patterns[1]);   // Melody: copied
    CHECK(before->tracks[0] == after->tracks[0]);       // tracks untouched
    CHECK(f.pattern(f.melody).notes.size() == 2);
}

TEST("a failed edit changes nothing and records no undo step") {
    Fixture f;
    const auto rev = f.session.revision();
    const auto depth = f.session.undoDepth();
    const ProjectPtr before = f.session.snapshot();

    // Second note is invalid: the whole batch must be rejected.
    auto r = f.session.perform("bad", [&](Draft& d) {
        return addNotes(d, f.melody, std::array{NoteSpec{60, 0, kSixteenth, 0.8f}, NoteSpec{200, 0, kSixteenth, 0.8f}});
    });
    CHECK(!r);
    CHECK(!r.error().message.empty());
    CHECK(f.session.revision() == rev);
    CHECK(f.session.undoDepth() == depth);
    CHECK(f.session.snapshot() == before);
}

TEST("notes stay sorted after add and move; moves are validated") {
    Fixture f;
    auto ids = f.add(f.melody, {{67, 3 * kSixteenth, kSixteenth, 0.8f}, {60, 0, kSixteenth, 0.8f}, {64, kSixteenth, kSixteenth, 0.8f}});
    CHECK(ids.size() == 3);
    CHECK(sortedNotes(f.pattern(f.melody)));
    CHECK(f.pattern(f.melody).notes.front().pitch == 60);

    auto moved = f.session.perform("Move", [&](Draft& d) { return moveNotes(d, f.melody, std::array{ids[1]}, 8 * kSixteenth, 2); });
    CHECK(moved);
    CHECK(sortedNotes(f.pattern(f.melody)));
    CHECK(f.pattern(f.melody).notes.back().pitch == 62);

    auto outOfRange = f.session.perform("Move", [&](Draft& d) { return moveNotes(d, f.melody, std::array{ids[0]}, 0, 100); });
    CHECK(!outOfRange);
    auto beforeStart = f.session.perform("Move", [&](Draft& d) { return moveNotes(d, f.melody, std::array{ids[0]}, -kTicksPerQuarter, 0); });
    CHECK(!beforeStart);
    auto missing = f.session.perform("Move", [&](Draft& d) { return moveNotes(d, f.melody, std::array{NoteId{999999}}, 0, 1); });
    CHECK(!missing);
}

TEST("undo and redo restore exact snapshots and carry names") {
    Fixture f;
    const ProjectPtr s0 = f.session.snapshot();
    f.add(f.melody, {{60, 0, kSixteenth, 0.8f}});
    const ProjectPtr s1 = f.session.snapshot();
    CHECK(f.session.undoName() == "Add Notes");

    CHECK(f.session.undo());
    CHECK(f.session.snapshot() == s0);
    CHECK(f.session.redoName() == "Add Notes");
    CHECK(f.session.redo());
    CHECK(f.session.snapshot() == s1);

    CHECK(f.session.undo());
    (void)f.session.perform("Tempo", [&](Draft& d) { return setTempo(d, 140); });
    CHECK(!f.session.canRedo());  // a new edit clears redo
}

TEST("a gesture records exactly one undo step; cancel restores the start") {
    Fixture f;
    auto ids = f.add(f.melody, {{60, 0, kSixteenth, 0.8f}});
    const auto depth = f.session.undoDepth();
    const ProjectPtr start = f.session.snapshot();

    CHECK(f.session.beginGesture("Move Note"));
    for (int i = 0; i < 50; ++i)
        (void)f.session.perform("drag", [&](Draft& d) { return moveNotes(d, f.melody, ids, kSixteenth, 0); });
    CHECK(!f.session.canUndo());  // history is locked during a gesture
    CHECK(f.session.endGesture());
    CHECK(f.session.undoDepth() == depth + 1);
    CHECK(f.session.undoName() == "Move Note");
    CHECK(f.pattern(f.melody).notes.front().start == 50 * kSixteenth);
    CHECK(f.session.undo());
    CHECK(f.session.snapshot() == start);

    CHECK(f.session.beginGesture("Move Note"));
    (void)f.session.perform("drag", [&](Draft& d) { return moveNotes(d, f.melody, ids, kSixteenth, 0); });
    CHECK(f.session.cancelGesture());
    CHECK(f.session.snapshot() == start);

    CHECK(f.session.beginGesture("Nothing"));
    CHECK(f.session.endGesture());
    CHECK(f.session.undoDepth() == depth);  // empty gestures leave no step
}

TEST("history limit drops the oldest steps") {
    Fixture f;
    f.session.setHistoryLimit(10);
    for (int i = 0; i < 25; ++i)
        (void)f.session.perform("Tempo", [&](Draft& d) { return setTempo(d, 100 + i); });
    CHECK(f.session.undoDepth() == 10);
}

TEST("clips: validated, sorted by start, only on instrument tracks") {
    Fixture f;
    auto place = [&](Tick start) {
        return f.session.perform("Clip", [&](Draft& d) { return placeClip(d, f.drums, f.beat, start, 4 * kTicksPerQuarter); });
    };
    CHECK(place(8 * kTicksPerQuarter));
    CHECK(place(0));
    CHECK(!place(-1));
    const Track* t = findTrack(*f.session.snapshot(), f.drums);
    CHECK(t->clips.size() == 2 && t->clips[0].start == 0);

    TrackId audio;
    (void)f.session.perform("Audio", [&](Draft& d) {
        auto r = addTrack(d, TrackKind::audio, "Vox");
        if (r) audio = r.value();
        return r;
    });
    auto onAudio = f.session.perform("Clip", [&](Draft& d) { return placeClip(d, audio, f.beat, 0, kTicksPerQuarter); });
    CHECK(!onAudio);
}

TEST("CoreAPI: edits, range queries and errors without exceptions") {
    auto* s = api::SessionHandle::create();
    auto track = s->addTrack("Synth", api::TrackKind::instrument);
    auto pattern = s->addPattern("Melody", 4 * api::ticksPerQuarter());
    CHECK(track != 0 && pattern != 0);

    api::NoteInputList in;
    for (int i = 0; i < 16; ++i)
        in.push_back({60 + i % 12, i * kSixteenth, kSixteenth, 0.7f});
    auto ids = s->addNotes(pattern, in);
    CHECK(ids.size() == 16);

    const auto view = s->snapshot();
    CHECK(view.noteCount(pattern) == 16);
    CHECK(view.notesInRange(pattern, 4 * kSixteenth, 8 * kSixteenth).size() == 4);
    // A long note that starts before the range but still sounds inside it is included.
    s->addNotes(pattern, {{48, 0, 16 * kSixteenth, 0.5f}});
    CHECK(s->snapshot().notesInRange(pattern, 4 * kSixteenth, 8 * kSixteenth).size() == 5);
    CHECK(view.noteCount(pattern) == 16);  // the old view is unaffected

    CHECK(s->addPattern("Bad", 0) == 0);
    CHECK(!s->lastError().empty());
    CHECK(!s->setTempo(5000));
    CHECK(s->setTempo(128));
    CHECK(s->lastError().empty());
    CHECK(s->undoName() == "Change Tempo");

    CHECK(s->placeClip(track, pattern, 0, 4 * api::ticksPerQuarter()) != 0);
    CHECK(s->snapshot().clips(track).size() == 1);
    jrb_session_release(s);
}

int main() { return jrbtest::runAll(); }
