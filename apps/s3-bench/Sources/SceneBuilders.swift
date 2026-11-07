import CoreGraphics
import CoreText
import JRBCore

/// Colours as SIMD for the display list.
@MainActor
enum Palette {
    static let surface0 = Theme.surface0.simd
    static let surface1 = Theme.surface1.simd
    static let surface2 = Theme.surface2.simd
    static let surface3 = Theme.surface3.simd
    static let blackKey = Theme.blackKey.simd
    static let gridMinor = Theme.gridMinor.simd
    static let gridBeat = Theme.gridBeat.simd
    static let gridBar = Theme.gridBar.simd
    static let textPrimary = Theme.textPrimary.simd
    static let textSecondary = Theme.textSecondary.simd
    static let accent = Theme.accent.simd
    static let separator = SIMD4<Float>(0, 0, 0, 0.3)
    static let noteLabel = SIMD4<Float>(10 / 255, 14 / 255, 22 / 255, 0.85)
    static let clipInk = SIMD4<Float>(10 / 255, 12 / 255, 18 / 255, 0.72)
    static let velocity = Theme.velocityColors.map(\.simd)
    static let tracks = Theme.trackColors.map(\.simd)
}

/// Snaps a coordinate to the device-pixel grid so hairlines stay crisp.
private func snap(_ v: CGFloat, _ scale: CGFloat) -> CGFloat { (v * scale).rounded() / scale }

// MARK: - Piano roll

extension PianoRollCanvas {
    /// Same content as draw(_:), emitted as a display list.
    func build(_ dl: inout DisplayList, size: CGSize, scale: CGFloat) -> Int {
        dl.reset()
        let kw = Self.keyboardWidth, rh = Self.rulerHeight, vh = Self.velocityHeight
        let grid = CGRect(x: kw, y: rh, width: size.width - kw, height: size.height - rh - vh)
        let endTick = originTick + Double(grid.width) * ticksPerPoint
        let hair = 1 / scale
        func rowY(_ pitch: Int) -> CGFloat { grid.minY + CGFloat(topPitch - Double(pitch)) * keyHeight }

        dl.rect(CGRect(origin: .zero, size: size), Palette.surface1)
        let firstRow = Int(floor(topPitch))
        let rowCount = Int(ceil(grid.height / keyHeight)) + 1
        dl.clip = grid
        for r in 0...rowCount {
            let pitch = firstRow - r
            if pitch >= 0 && pitch <= 127 && NoteNames.isBlack(pitch) {
                dl.rect(CGRect(x: grid.minX, y: rowY(pitch), width: grid.width, height: keyHeight), Palette.surface0)
            }
        }

        let barTicks = ppq * 4
        var stepTicks = ppq / 4
        while stepTicks / ticksPerPoint < 10 { stepTicks *= 2 }
        var t = floor(originTick / stepTicks) * stepTicks
        while t <= endTick {
            let x = snap(grid.minX + CGFloat((t - originTick) / ticksPerPoint), scale)
            let color = t.truncatingRemainder(dividingBy: barTicks) == 0 ? Palette.gridBar
                : t.truncatingRemainder(dividingBy: ppq) == 0 ? Palette.gridBeat : Palette.gridMinor
            dl.rect(CGRect(x: x, y: grid.minY, width: hair, height: grid.height), color)
            t += stepTicks
        }
        for r in 0...rowCount where (firstRow - r) % 12 == 0 {
            dl.rect(CGRect(x: grid.minX, y: snap(rowY(firstRow - r) + keyHeight, scale), width: grid.width, height: hair),
                    Palette.gridBar)
        }

        let notes = snapshot.notesInRange(patternId, Int64(originTick), Int64(ceil(endTick)))
        let lowestVisible = Double(firstRow - rowCount)
        var drawn = 0
        var stems: [(CGFloat, Float)] = []
        stems.reserveCapacity(notes.count)
        for note in notes {
            let x = grid.minX + CGFloat((Double(note.start) - originTick) / ticksPerPoint)
            stems.append((x, note.velocity))
            guard Double(note.pitch) <= topPitch + 1, Double(note.pitch) >= lowestVisible else { continue }
            let width = max(2, CGFloat(Double(note.length) / ticksPerPoint) - 1)
            let rect = CGRect(x: x, y: rowY(Int(note.pitch)) + 1, width: width, height: keyHeight - 2)
            dl.rect(rect, Palette.velocity[min(7, max(0, Int(note.velocity * 8)))], radius: width > 6 ? 2.5 : 0)
            drawn += 1
        }
        if keyHeight >= 12 {
            for note in notes where Double(note.length) / ticksPerPoint > 31 {
                guard Double(note.pitch) <= topPitch + 1, Double(note.pitch) >= lowestVisible else { continue }
                let x = grid.minX + CGFloat((Double(note.start) - originTick) / ticksPerPoint)
                dl.text(NoteNames.name(Int(note.pitch)), .small,
                        at: CGPoint(x: x + 4, y: rowY(Int(note.pitch)) + keyHeight - 4), Palette.noteLabel)
            }
        }

        // Velocity lane.
        dl.clip = DisplayList.noClip
        dl.rect(CGRect(x: 0, y: grid.maxY, width: size.width, height: vh), Palette.surface2)
        dl.clip = CGRect(x: grid.minX, y: grid.maxY, width: grid.width, height: vh)
        let laneTop = grid.maxY + 8, laneHeight = vh - 16
        for (x, velocity) in stems {
            let top = laneTop + laneHeight * CGFloat(1 - velocity)
            dl.rect(CGRect(x: x, y: top, width: 2, height: laneTop + laneHeight - top), Palette.accent, radius: 1)
        }

        // Keyboard.
        dl.clip = CGRect(x: 0, y: grid.minY, width: kw, height: grid.height)
        dl.rect(CGRect(x: 0, y: grid.minY, width: kw, height: grid.height), Palette.surface3)
        for r in 0...rowCount {
            let pitch = firstRow - r
            guard pitch >= 0 && pitch <= 127 else { continue }
            let y = rowY(pitch)
            if NoteNames.isBlack(pitch) {
                dl.rect(CGRect(x: 0, y: y, width: kw * 0.62, height: keyHeight), Palette.blackKey, radius: 1.5)
            } else {
                dl.rect(CGRect(x: 0, y: snap(y + keyHeight, scale), width: kw, height: hair), Palette.separator)
            }
            if pitch % 12 == 0 && keyHeight >= 8 {
                dl.text(NoteNames.name(pitch), .small, at: CGPoint(x: kw - 26, y: y + keyHeight - 3), Palette.textSecondary)
            }
        }

        // Ruler.
        dl.clip = DisplayList.noClip
        dl.rect(CGRect(x: 0, y: 0, width: size.width, height: rh), Palette.surface2)
        var labelBars = 1
        while Double(labelBars) * barTicks / ticksPerPoint < 48 { labelBars *= 2 }
        var b = floor(originTick / barTicks / Double(labelBars)) * Double(labelBars)
        while b * barTicks <= endTick {
            let x = grid.minX + CGFloat((b * barTicks - originTick) / ticksPerPoint)
            if x >= grid.minX - 1 { dl.text("\(Int(b) + 1)", .ui, at: CGPoint(x: x + 3, y: 16), Palette.textSecondary) }
            b += Double(labelBars)
        }
        return drawn
    }
}

// MARK: - Arrangement

/// Pattern previews as 8-bit alpha masks, packed later into one GPU atlas.
@MainActor
final class PreviewMasks {
    static let width = 512, height = 64
    private(set) var slots: [UInt64: Int] = [:]
    private(set) var pixels: [[UInt8]] = []

    init(project: BenchProject) {
        let snapshot = project.snapshot
        for p in 0..<snapshot.patternCount() {
            let pattern = snapshot.patternAt(p)
            guard pattern.id != project.bigPattern else { continue }
            var bytes = [UInt8](repeating: 0, count: Self.width * Self.height)
            let notes = snapshot.notes(pattern.id)
            var lo: Int32 = 127, hi: Int32 = 0
            for n in notes { lo = min(lo, n.pitch); hi = max(hi, n.pitch) }
            let range = max(12, Int(hi - lo) + 1)
            let rowH = max(1, Self.height / range)
            for n in notes {
                let x0 = Int(Double(n.start) / Double(pattern.length) * Double(Self.width))
                let x1 = max(x0 + 1, Int(Double(n.start + n.length) / Double(pattern.length) * Double(Self.width)))
                let y0 = Self.height - (Int(n.pitch - lo) + 1) * Self.height / range
                for y in max(0, y0)..<min(Self.height, y0 + rowH) {
                    for x in max(0, x0)..<min(Self.width, x1) { bytes[y * Self.width + x] = 255 }
                }
            }
            slots[pattern.id] = pixels.count
            pixels.append(bytes)
        }
    }
}

extension ArrangementCanvas {
    func build(_ dl: inout DisplayList, size: CGSize, scale: CGFloat, previews: PreviewMasks) -> Int {
        dl.reset()
        let hw = Self.headerWidth, rh = Self.rulerHeight
        let lanes = CGRect(x: hw, y: rh, width: size.width - hw, height: size.height - rh)
        let endTick = originTick + Double(lanes.width) * ticksPerPoint
        let rows = project.rows
        let firstRow = max(0, Int(scrollY / trackHeight))
        let lastRow = min(rows.count - 1, Int((scrollY + lanes.height) / trackHeight))
        let hair = 1 / scale
        func laneY(_ r: Int) -> CGFloat { lanes.minY + CGFloat(r) * trackHeight - scrollY }

        dl.rect(CGRect(origin: .zero, size: size), Palette.surface0)
        dl.clip = lanes
        if firstRow <= lastRow {
            for r in firstRow...lastRow where r % 2 == 1 {
                dl.rect(CGRect(x: lanes.minX, y: laneY(r), width: lanes.width, height: trackHeight), Palette.surface1)
            }
        }
        let barTicks = ppq * 4
        var stepBars = 1.0
        while stepBars * barTicks / ticksPerPoint < 12 { stepBars *= 2 }
        var b = floor(originTick / barTicks / stepBars) * stepBars
        while b * barTicks <= endTick {
            let x = snap(lanes.minX + CGFloat((b * barTicks - originTick) / ticksPerPoint), scale)
            dl.rect(CGRect(x: x, y: lanes.minY, width: hair, height: lanes.height), Palette.gridBeat)
            b += stepBars
        }

        var visible = 0
        let showContent = trackHeight >= 28
        let secondsPerPoint = ticksPerPoint / ppq * 0.5     // 120 BPM; the real app uses the tempo map
        if firstRow <= lastRow {
            for r in firstRow...lastRow {
                let row = rows[r]
                let y = laneY(r)
                let body = Palette.tracks[row.colorIndex % Palette.tracks.count] * SIMD4(1, 1, 1, 0.85)
                func clipRect(_ start: Int64, _ length: Int64) -> CGRect? {
                    let s = Double(start), e = Double(start + length)
                    guard e > originTick && s < endTick else { return nil }
                    let x0 = lanes.minX + CGFloat((s - originTick) / ticksPerPoint)
                    let x1 = lanes.minX + CGFloat((e - originTick) / ticksPerPoint)
                    return CGRect(x: x0, y: y + 1, width: max(2, x1 - x0 - 1), height: trackHeight - 2)
                }
                for clip in row.clips {
                    guard let rect = clipRect(clip.start, clip.length) else { continue }
                    visible += 1
                    dl.rect(rect, body, radius: 3)
                    if showContent, let slot = previews.slots[clip.patternId] {
                        dl.mask(CGRect(x: rect.minX + 2, y: rect.minY + Self.clipTitleHeight, width: rect.width - 4,
                                       height: rect.height - Self.clipTitleHeight - 2), slot: slot, Palette.clipInk)
                    }
                    if rect.width > 48 && trackHeight >= 20 {
                        dl.text(row.name, .small, at: CGPoint(x: max(rect.minX, lanes.minX) + 4, y: rect.minY + 11), Palette.noteLabel)
                    }
                }
                for clip in row.audioClips {
                    guard let rect = clipRect(clip.start, clip.length) else { continue }
                    visible += 1
                    dl.rect(rect, body, radius: 3)
                    appendWaveform(clip.waveform, index: clip.waveformIndex, rect: rect, secondsPerPoint: secondsPerPoint,
                                   scale: scale, into: &dl)
                    if rect.width > 48 && trackHeight >= 20 {
                        dl.text(row.name, .small, at: CGPoint(x: max(rect.minX, lanes.minX) + 4, y: rect.minY + 11), Palette.noteLabel)
                    }
                }
            }
        }

        // Headers.
        dl.clip = CGRect(x: 0, y: lanes.minY, width: hw, height: lanes.height)
        dl.rect(CGRect(x: 0, y: lanes.minY, width: hw, height: lanes.height), Palette.surface2)
        if firstRow <= lastRow {
            for r in firstRow...lastRow {
                let y = laneY(r)
                dl.rect(CGRect(x: 0, y: y, width: 4, height: trackHeight), Palette.tracks[rows[r].colorIndex % Palette.tracks.count])
                dl.rect(CGRect(x: 0, y: snap(y + trackHeight, scale), width: hw, height: hair), Palette.separator)
                if trackHeight >= 18 {
                    dl.text(rows[r].name, .ui, at: CGPoint(x: 14, y: y + min(trackHeight / 2 + 4, 20)), Palette.textPrimary)
                }
            }
        }

        // Ruler.
        dl.clip = DisplayList.noClip
        dl.rect(CGRect(x: 0, y: 0, width: size.width, height: rh), Palette.surface2)
        var labelBars = 1.0
        while labelBars * barTicks / ticksPerPoint < 40 { labelBars *= 2 }
        var lb = floor(originTick / barTicks / labelBars) * labelBars
        while lb * barTicks <= endTick {
            let x = lanes.minX + CGFloat((lb * barTicks - originTick) / ticksPerPoint)
            if x >= lanes.minX - 1 { dl.text("\(Int(lb) + 1)", .ui, at: CGPoint(x: x + 3, y: 16), Palette.textSecondary) }
            lb += labelBars
        }
        return visible
    }

    /// One item per clip. The level is the coarsest that still has at least one
    /// peak per device-pixel column; the GPU reduces the few peaks per column.
    private func appendWaveform(_ waveform: Waveform, index: Int, rect: CGRect, secondsPerPoint: Double, scale: CGFloat,
                                into dl: inout DisplayList) {
        let body = CGRect(x: rect.minX, y: rect.minY + Self.clipTitleHeight, width: rect.width,
                          height: rect.height - Self.clipTitleHeight - 2)
        guard body.height > 6 else { return }
        let samplesPerColumn = secondsPerPoint / Double(scale) * waveform.sampleRate
        var level = 0
        var samplesPerPeak = Double(Waveform.baseSamplesPerPeak)
        while level + 1 < waveform.levels.count && samplesPerPeak * 4 <= samplesPerColumn {
            level += 1
            samplesPerPeak *= 4
        }
        dl.wave(DisplayList.Wave(rect: body, color: Palette.clipInk, clip: .zero, waveform: index, level: level,
                                 peakAtLeftEdge: 0, peaksPerPoint: Float(secondsPerPoint * waveform.sampleRate / samplesPerPeak)))
    }
}
