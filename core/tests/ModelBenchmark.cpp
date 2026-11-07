// Spike S1 measurement: cost of snapshot edits and undo history on a large project
// (200 tracks, 200 patterns x 500 notes = 100k notes, plus one 20k-note pattern).

#include "jrb/edit/Commands.hpp"
#include "jrb/edit/Session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

#include <mach/mach.h>

using namespace jrb;
using Clock = std::chrono::steady_clock;

namespace {

double residentMB() {
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
}

template <class Fn>
double medianMicros(int runs, Fn&& fn) {
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        auto t0 = Clock::now();
        fn(i);
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    std::nth_element(samples.begin(), samples.begin() + runs / 2, samples.end());
    return samples[static_cast<std::size_t>(runs / 2)];
}

} // namespace

int main() {
    const double mbStart = residentMB();
    Session session;
    session.setHistoryLimit(500);

    std::vector<PatternId> patterns;
    std::vector<NoteId> firstNotes;
    PatternId big;
    NoteId bigFirst;

    auto t0 = Clock::now();
    (void)session.perform("build", [&](Draft& d) -> Status {
        for (int t = 0; t < 200; ++t) {
            auto track = addTrack(d, TrackKind::instrument, "Track " + std::to_string(t)).value();
            auto pattern = addPattern(d, "Pattern", 64 * kTicksPerQuarter).value();
            std::vector<NoteSpec> notes;
            for (int n = 0; n < 500; ++n)
                notes.push_back({36 + (n * 7) % 48, n * kTicksPerQuarter / 8, kTicksPerQuarter / 8, 0.8f});
            firstNotes.push_back(addNotes(d, pattern, notes).value().front());
            patterns.push_back(pattern);
            for (int c = 0; c < 10; ++c)
                (void)placeClip(d, track, pattern, c * 64 * kTicksPerQuarter, 64 * kTicksPerQuarter);
        }
        big = addPattern(d, "Big", 2560 * kTicksPerQuarter).value();
        std::vector<NoteSpec> notes;
        for (int n = 0; n < 20000; ++n)
            notes.push_back({24 + (n * 5) % 72, n * kTicksPerQuarter / 8, kTicksPerQuarter / 8, 0.8f});
        bigFirst = addNotes(d, big, notes).value().front();
        return ok();
    });
    const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const double mbBuilt = residentMB();

    std::printf("History after build: %.1f KB\n", static_cast<double>(session.historyBytes()) / 1024.0);
    const double moveSmall = medianMicros(500, [&](int i) {
        auto p = patterns[static_cast<std::size_t>(i % 200)];
        auto n = firstNotes[static_cast<std::size_t>(i % 200)];
        (void)session.perform("Move", [&](Draft& d) { return moveNotes(d, p, std::array{n}, 0, (i % 2) ? -1 : 1); });
    });
    const double mbAfterSmall = residentMB();
    const double histSmall = static_cast<double>(session.historyBytes()) / (1024.0 * 1024.0);

    const double moveBig = medianMicros(500, [&](int i) {
        (void)session.perform("Move", [&](Draft& d) { return moveNotes(d, big, std::array{bigFirst}, 0, (i % 2) ? -1 : 1); });
    });
    const double mbAfterBig = residentMB();
    const double histBig = static_cast<double>(session.historyBytes()) / (1024.0 * 1024.0);

    const double tempo = medianMicros(500, [&](int i) {
        (void)session.perform("Tempo", [&](Draft& d) { return setTempo(d, 100.0 + i % 50); });
    });

    const double undo = medianMicros(400, [&](int) { (void)session.undo(); });
    const double redo = medianMicros(400, [&](int) { (void)session.redo(); });

    std::printf("Project: 200 tracks, 2000 clips, 100,000 + 20,000 notes\n");
    std::printf("Build (one transaction):              %8.1f ms\n", buildMs);
    std::printf("Move 1 note, 500-note pattern:        %8.1f us (median)\n", moveSmall);
    std::printf("Move 1 note, 20k-note pattern:        %8.1f us (median)\n", moveBig);
    std::printf("Change tempo (metadata only):         %8.1f us (median)\n", tempo);
    std::printf("Undo / redo:                          %8.2f / %.2f us (median)\n", undo, redo);
    std::printf("Resident memory: start %.1f MB, built %.1f MB, +500 small-edit steps %.1f MB, +500 big-edit steps %.1f MB\n",
                mbStart, mbBuilt, mbAfterSmall, mbAfterBig);
    std::printf("Undo history estimate: after 500 small edits %.1f MB, after 500 big edits %.1f MB\n", histSmall, histBig);
    std::printf("Undo depth: %zu (limit 500 steps / 256 MB)\n", session.undoDepth());
    return 0;
}
