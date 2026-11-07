// Spike S1 — Swift ↔ C++ core through Swift's C++ interop.
// Exercises CoreAPI exactly as the app's Bridge layer will, checks results,
// and times the boundary. Exit code is non-zero if any check fails.

import Foundation
import JRBCore

typealias Core = jrb.api

nonisolated(unsafe) var failures = 0  // single-threaded test driver
@MainActor func check(_ condition: Bool, _ message: String, line: Int = #line) {
    if !condition {
        failures += 1
        print("FAIL (line \(line)): \(message)")
    }
}

func micros(_ body: () -> Void) -> Double {
    let start = ContinuousClock.now
    body()
    let d = ContinuousClock.now - start
    return Double(d.components.seconds) * 1e6 + Double(d.components.attoseconds) / 1e12
}

// MARK: - A thin Swift-side wrapper, as the Bridge module would provide

struct NoteSpec {
    var pitch: Int32
    var start: Int64
    var length: Int64
    var velocity: Float
}

extension Core.SessionHandle {
    func addNotes(pattern: UInt64, _ notes: [NoteSpec]) -> [UInt64] {
        var input = Core.NoteInputList()
        input.reserve(notes.count)
        for n in notes {
            var item = Core.NoteInput()
            item.pitch = n.pitch
            item.start = n.start
            item.length = n.length
            item.velocity = n.velocity
            input.push_back(item)
        }
        return Array(addNotes(pattern, input))
    }

    var errorMessage: String { String(lastError()) }
}

// MARK: - Functional checks

let ppq = Core.ticksPerQuarter()
let sixteenth = ppq / 4
check(ppq == 6720, "PPQ is 6720")

let session: Core.SessionHandle = Core.SessionHandle.create()

let drums = session.addTrack(std.string("Drums"), .instrument)
let synth = session.addTrack(std.string("Synth"), .instrument)
let melody = session.addPattern(std.string("Melody"), 4 * ppq)
check(drums != 0 && synth != 0 && melody != 0, "tracks and pattern created")

let ids = session.addNotes(pattern: melody, [
    NoteSpec(pitch: 60, start: 0, length: sixteenth, velocity: 0.8),
    NoteSpec(pitch: 64, start: sixteenth, length: sixteenth, velocity: 0.7),
    NoteSpec(pitch: 67, start: 2 * sixteenth, length: sixteenth, velocity: 0.6),
])
check(ids.count == 3, "three notes added")
check(String(session.undoName()) == "Add Notes", "undo name crosses the boundary")

let before = session.snapshot()
var moveIds = Core.IdList()
moveIds.push_back(ids[0])
check(session.moveNotes(melody, moveIds, 4 * sixteenth, 12), "move succeeds")
let after = session.snapshot()

check(before.noteCount(melody) == 3 && after.noteCount(melody) == 3, "counts")
check(before.notes(melody).first?.pitch == 60, "old snapshot is unchanged")
check(after.notes(melody).last?.pitch == 72, "new snapshot has the moved note, sorted last")
check(!before.isIdentical(after), "distinct snapshots")
check(after.revision() > before.revision(), "revision increases")

// Errors surface as values, never as crashes.
check(!session.setTempo(2000), "invalid tempo rejected")
check(!session.errorMessage.isEmpty, "error message available: \(session.errorMessage)")
check(session.addPattern(std.string("Zero"), 0) == 0, "zero-length pattern rejected")

// Gesture: 30 drag steps → one undo step.
check(session.beginGesture(std.string("Move Note")), "gesture begins")
for _ in 0..<30 { _ = session.moveNotes(melody, moveIds, sixteenth, 0) }
check(session.endGesture(), "gesture ends")
check(String(session.undoName()) == "Move Note", "gesture is one named step")
check(session.undo(), "undo gesture")
check(session.snapshot().isIdentical(after), "undo restores the exact snapshot")

check(session.placeClip(synth, melody, 0, 4 * ppq) != 0, "clip placed")
check(session.snapshot().clips(synth).count == 1, "clip visible to Swift")

let view = session.snapshot()
var names: [String] = []
for i in 0..<view.trackCount() { names.append(String(view.trackAt(i).name)) }
check(names == ["Drums", "Synth"], "track names: \(names)")

// Snapshots are Sendable values that can be handed to another thread
// (the graph-compiler and autosave paths). This must compile in Swift 6 mode.
let handoff = view
let offThreadCount = await Task.detached { [handoff, melody] in
    handoff.notes(melody).count
}.value
check(offThreadCount == 3, "snapshot readable off the main thread")

// MARK: - Boundary timing

let big = session.addPattern(std.string("Big"), 2560 * ppq)
var bulk: [NoteSpec] = []
for n in 0..<20_000 {
    bulk.append(NoteSpec(pitch: Int32(24 + (n * 5) % 72), start: Int64(n) * ppq / 8, length: ppq / 8, velocity: 0.8))
}
var bigIds: [UInt64] = []
let tBulkAdd = micros { bigIds = session.addNotes(pattern: big, bulk) }

var single: [Double] = []
for i in 0..<200 {
    let t = micros { _ = session.addNotes(pattern: melody, [NoteSpec(pitch: 60, start: Int64(i) * sixteenth, length: sixteenth, velocity: 0.8)]) }
    single.append(t)
}
single.sort()

var readAll = 0.0, readVisible = 0.0, snap = 0.0
var visibleCount = 0
for _ in 0..<50 {
    var v = Core.ProjectView()
    snap += micros { v = session.snapshot() }
    readAll += micros { _ = v.notes(big).count }
    // A typical visible piano-roll region: 16 bars.
    readVisible += micros { visibleCount = v.notesInRange(big, 0, 64 * ppq).count }
}

check(bigIds.count == 20_000, "bulk add")
print("S1 interop timings (Swift side, release build)")
print(String(format: "  add 20,000 notes in one call:         %8.0f us", tBulkAdd))
print(String(format: "  add 1 note (median of 200):           %8.1f us", single[100]))
print(String(format: "  take snapshot (avg):                  %8.2f us", snap / 50))
print(String(format: "  read all 20,000 notes (avg):          %8.0f us", readAll / 50))
print(String(format: "  read visible 16 bars, %4d notes (avg):%8.1f us", visibleCount, readVisible / 50))

print(failures == 0 ? "S1 interop: all checks passed" : "S1 interop: \(failures) checks FAILED")
exit(failures == 0 ? 0 : 1)
