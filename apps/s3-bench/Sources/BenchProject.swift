import Foundation
import JRBCore

typealias Core = jrb.api

/// Builds the benchmark project through the real CoreAPI, then exposes a
/// Swift-side cache of what the arrangement needs (rebuilt per snapshot
/// revision, as the app's view-models will).
@MainActor
final class BenchProject {
    struct TrackRow {
        let id: UInt64
        let name: String
        let colorIndex: Int
        let isAudio: Bool
        let clips: [Core.ClipData]
        let audioClips: [AudioClip]
    }

    /// Audio clips are synthetic: the model has no audio clips yet (Phase 4).
    struct AudioClip {
        let start: Int64
        let length: Int64
        let waveform: Waveform
        let waveformIndex: Int
    }

    let session: Core.SessionHandle = Core.SessionHandle.create()
    let ppq = Core.ticksPerQuarter()
    private(set) var bigPattern: UInt64 = 0
    private(set) var rows: [TrackRow] = []
    private(set) var songLength: Int64 = 0
    private(set) var patternNoteCounts: [UInt64: Int] = [:]
    private(set) var waveforms: [Waveform] = []

    var snapshot: Core.ProjectView { session.snapshot() }

    init(instrumentTracks: Int = 160, audioTracks: Int = 40, notesPerPattern: Int = 500, clipsPerTrack: Int = 10,
         bigPatternNotes: Int = 20_000) {
        var rng = SplitMix(seed: 2026)
        let clipLength = 64 * ppq
        var trackIds: [UInt64] = []

        for t in 0..<(instrumentTracks + audioTracks) {
            let isAudio = t >= instrumentTracks
            let id = session.addTrack(std.string(isAudio ? "Audio \(t - instrumentTracks + 1)" : "Track \(t + 1)"),
                                      isAudio ? .audio : .instrument)
            trackIds.append(id)
            guard !isAudio else { continue }
            let pattern = session.addPattern(std.string("Pattern \(t + 1)"), clipLength)
            var notes = Core.NoteInputList()
            notes.reserve(notesPerPattern)
            let root = Int32(36 + (t * 5) % 36)
            for n in 0..<notesPerPattern {
                var note = Core.NoteInput()
                note.pitch = root + Int32([0, 3, 7, 10, 12, 15, 19][n % 7]) + Int32(rng.next(3))
                note.start = Int64(n) * clipLength / Int64(notesPerPattern)
                note.length = ppq / 8 * Int64(1 + rng.next(3))
                note.velocity = 0.3 + Float(rng.next(70)) / 100
                notes.push_back(note)
            }
            _ = session.addNotes(pattern, notes)
            for c in 0..<clipsPerTrack where rng.next(10) < 8 || c == 0 {
                _ = session.placeClip(id, pattern, Int64(c) * clipLength, clipLength)
            }
        }

        // The piano-roll pattern: 20,000 notes of chords and melody across 640 bars.
        bigPattern = session.addPattern(std.string("Big"), 2560 * ppq)
        var big = Core.NoteInputList()
        big.reserve(bigPatternNotes)
        let step = 2560 * ppq / Int64(bigPatternNotes / 4)
        for i in 0..<(bigPatternNotes / 4) {
            let start = Int64(i) * step
            let chordRoot = Int32(48 + [0, 5, 7, 3, 8, 10][(i / 8) % 6])
            for (k, interval) in [0, 4, 7].enumerated() {
                var note = Core.NoteInput()
                note.pitch = chordRoot + Int32(interval)
                note.start = start
                note.length = step - ppq / 16
                note.velocity = 0.45 + Float(k) * 0.1
                big.push_back(note)
            }
            var melody = Core.NoteInput()
            melody.pitch = 72 + Int32(rng.next(15))
            melody.start = start + ppq / 2 * Int64(rng.next(2))
            melody.length = ppq / 4 * Int64(1 + rng.next(4))
            melody.velocity = 0.5 + Float(rng.next(50)) / 100
            big.push_back(melody)
        }
        _ = session.addNotes(bigPattern, big)

        // Swift-side view-model cache.
        waveforms = (0..<4).map { Waveform.synthetic(seed: UInt64($0 + 1), seconds: 180) }
        let view = snapshot
        for i in 0..<view.trackCount() {
            let track = view.trackAt(i)
            let isAudio = track.kind == .audio
            var audioClips: [AudioClip] = []
            if isAudio {
                // 180-second clips at 120 BPM = 360 quarters, with gaps.
                for c in 0..<4 {
                    let w = (Int(i) + c) % waveforms.count
                    audioClips.append(AudioClip(start: Int64(c) * 400 * ppq, length: 360 * ppq, waveform: waveforms[w],
                                                waveformIndex: w))
                }
            }
            rows.append(TrackRow(id: track.id, name: String(track.name), colorIndex: Int(track.colorIndex),
                                 isAudio: isAudio, clips: Array(view.clips(track.id)), audioClips: audioClips))
        }
        songLength = rows.flatMap { $0.clips.map { $0.start + $0.length } + $0.audioClips.map { $0.start + $0.length } }.max() ?? 0
        for p in 0..<view.patternCount() {
            let data = view.patternAt(p)
            patternNoteCounts[data.id] = Int(data.noteCount)
        }
    }
}

/// Precomputed min/max peak pyramid, as the real app will build in the background
/// from audio files: level 0 = 256 samples per peak, each level 4x coarser.
struct Waveform: Sendable {
    static let baseSamplesPerPeak = 256
    let sampleRate: Double
    let levels: [[(Float, Float)]]

    static func synthetic(seed: UInt64, seconds: Double, sampleRate: Double = 48_000) -> Waveform {
        var rng = SplitMix(seed: seed)
        let count = Int(seconds * sampleRate) / baseSamplesPerPeak
        var base = [(Float, Float)]()
        base.reserveCapacity(count)
        for i in 0..<count {
            let t = Double(i) / Double(count)
            let beat = 0.55 + 0.45 * abs(sin(t * 1400))           // transients
            let section = 0.4 + 0.6 * abs(sin(t * 9 + Double(seed)))  // song sections
            let amp = Float(beat * section) * (0.7 + 0.3 * Float(rng.next(100)) / 100)
            base.append((-amp * (0.85 + 0.15 * Float(rng.next(100)) / 100), amp))
        }
        var levels = [base]
        while levels.last!.count > 64 {
            let prev = levels.last!
            var next = [(Float, Float)]()
            next.reserveCapacity(prev.count / 4 + 1)
            var j = 0
            while j < prev.count {
                var lo: Float = 0, hi: Float = 0
                for k in j..<min(j + 4, prev.count) {
                    lo = min(lo, prev[k].0)
                    hi = max(hi, prev[k].1)
                }
                next.append((lo, hi))
                j += 4
            }
            levels.append(next)
        }
        return Waveform(sampleRate: sampleRate, levels: levels)
    }
}

struct SplitMix {
    var state: UInt64
    init(seed: UInt64) { state = seed }
    mutating func next(_ bound: Int) -> Int {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return Int((z ^ (z >> 31)) % UInt64(bound))
    }
}
