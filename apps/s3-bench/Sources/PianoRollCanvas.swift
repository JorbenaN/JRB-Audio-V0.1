import AppKit
import JRBCore

/// Core Graphics piano roll (ADR-015). Draws only what is visible, from a range
/// query on the immutable snapshot, with batched fills: one path per velocity
/// colour, one stroke per grid level.
@MainActor
final class PianoRollCanvas: NSView {
    var snapshot: Core.ProjectView
    let patternId: UInt64
    let meter: FrameMeter
    let ppq: Double

    /// Viewport.
    var originTick: Double = 0        // musical time at the left edge of the grid
    var ticksPerPoint: Double = 60
    var topPitch: Double = 96         // pitch row at the top edge
    var keyHeight: CGFloat = 14

    static let keyboardWidth: CGFloat = 64
    static let rulerHeight: CGFloat = 24
    static let velocityHeight: CGFloat = 88

    private let labels = TextCache(font: Theme.smallFont, color: Theme.rgb(10, 14, 22, 0.85))
    private let keyLabels = TextCache(font: Theme.smallFont, color: Theme.textSecondary)
    private let rulerLabels = TextCache(font: Theme.uiFont, color: Theme.textSecondary)

    init(snapshot: Core.ProjectView, patternId: UInt64, ppq: Int64, meter: FrameMeter) {
        self.snapshot = snapshot
        self.patternId = patternId
        self.ppq = Double(ppq)
        self.meter = meter
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
        layer?.isOpaque = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }

    var gridRect: CGRect {
        CGRect(x: Self.keyboardWidth, y: Self.rulerHeight, width: bounds.width - Self.keyboardWidth,
               height: bounds.height - Self.rulerHeight - Self.velocityHeight)
    }

    var visibleTicks: Double { Double(gridRect.width) * ticksPerPoint }

    override func draw(_ dirtyRect: NSRect) {
        let start = CACurrentMediaTime()
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let grid = gridRect
        let endTick = originTick + visibleTicks

        // Background and black-key rows.
        ctx.setFillColor(Theme.surface1)
        ctx.fill(bounds)
        ctx.setFillColor(Theme.surface0)
        var blackRows: [CGRect] = []
        let firstRow = Int(floor(topPitch))
        let rowCount = Int(ceil(grid.height / keyHeight)) + 1
        for r in 0...rowCount {
            let pitch = firstRow - r
            guard pitch >= 0 && pitch <= 127 else { continue }
            if NoteNames.isBlack(pitch) {
                blackRows.append(CGRect(x: grid.minX, y: rowY(pitch), width: grid.width, height: keyHeight))
            }
        }
        ctx.fill(blackRows)

        // Grid: subdivide until lines would be closer than 10 pt.
        ctx.saveGState()
        ctx.clip(to: grid)
        let barTicks = ppq * 4
        var stepTicks = ppq / 4
        while stepTicks / ticksPerPoint < 10 { stepTicks *= 2 }
        let minor = CGMutablePath(), beat = CGMutablePath(), bar = CGMutablePath()
        var t = floor(originTick / stepTicks) * stepTicks
        while t <= endTick {
            let x = grid.minX + CGFloat((t - originTick) / ticksPerPoint)
            let target = t.truncatingRemainder(dividingBy: barTicks) == 0 ? bar
                : t.truncatingRemainder(dividingBy: ppq) == 0 ? beat : minor
            target.move(to: CGPoint(x: x, y: grid.minY))
            target.addLine(to: CGPoint(x: x, y: grid.maxY))
            t += stepTicks
        }
        for r in 0...rowCount where (firstRow - r) % 12 == 0 {   // octave lines under each C
            let y = rowY(firstRow - r) + keyHeight
            bar.move(to: CGPoint(x: grid.minX, y: y))
            bar.addLine(to: CGPoint(x: grid.maxX, y: y))
        }
        let tuned = RenderOptions.tuned
        ctx.setShouldAntialias(!tuned)
        if tuned {
            // Opaque hairlines, one device pixel wide, filled as rectangles.
            let hair = 1 / (window?.backingScaleFactor ?? 2)
            for (path, color) in [(minor, Theme.gridMinor), (beat, Theme.gridBeat), (bar, Theme.gridBar)] {
                ctx.setFillColor(color.flattened(over: Theme.surface0))
                var rects: [CGRect] = []
                path.applyWithBlock { element in
                    if element.pointee.type == .moveToPoint {
                        let p = element.pointee.points[0]
                        rects.append(p.y == grid.minY ? CGRect(x: p.x, y: grid.minY, width: hair, height: grid.height)
                                                      : CGRect(x: grid.minX, y: p.y, width: grid.width, height: hair))
                    }
                }
                ctx.fill(rects)
            }
        } else {
            ctx.setLineWidth(1)
            for (path, color) in [(minor, Theme.gridMinor), (beat, Theme.gridBeat), (bar, Theme.gridBar)] {
                ctx.addPath(path)
                ctx.setStrokeColor(color)
                ctx.strokePath()
            }
        }

        // Notes: one range query, one path per velocity colour.
        let notes = snapshot.notesInRange(patternId, Int64(originTick), Int64(ceil(endTick)))
        var buckets = Array(repeating: CGMutablePath(), count: 8)
        for i in 0..<8 { buckets[i] = CGMutablePath() }
        let stems = CGMutablePath()
        var labelled: [(String, CGPoint)] = []
        let lowestVisible = Double(firstRow - rowCount)
        let velocityTop = grid.maxY + 8
        let velocityHeight = Self.velocityHeight - 16
        var drawn = 0
        for note in notes {
            let x = grid.minX + CGFloat((Double(note.start) - originTick) / ticksPerPoint)
            let width = max(2, CGFloat(Double(note.length) / ticksPerPoint) - 1)
            let velocityBucket = min(7, max(0, Int(note.velocity * 8)))
            // Velocity lane stem (every visible note, regardless of pitch).
            let stemTop = velocityTop + velocityHeight * CGFloat(1 - note.velocity)
            stems.move(to: CGPoint(x: x + 1, y: velocityTop + velocityHeight))
            stems.addLine(to: CGPoint(x: x + 1, y: stemTop))
            guard Double(note.pitch) <= topPitch + 1, Double(note.pitch) >= lowestVisible else { continue }
            let rect = CGRect(x: x, y: rowY(Int(note.pitch)) + 1, width: width, height: keyHeight - 2)
            if width > 6 && keyHeight > 6 && !tuned {
                buckets[velocityBucket].addRoundedRect(in: rect, cornerWidth: 2.5, cornerHeight: 2.5)
            } else {
                buckets[velocityBucket].addRect(rect)
            }
            if width > 30 && keyHeight >= 12 {
                labelled.append((NoteNames.name(Int(note.pitch)), CGPoint(x: x + 4, y: rect.maxY - 3)))
            }
            drawn += 1
        }
        for (i, path) in buckets.enumerated() where !path.isEmpty {
            ctx.addPath(path)
            ctx.setFillColor(Theme.velocityColors[i])
            ctx.fillPath()
        }
        ctx.setShouldAntialias(true)
        for (text, point) in labelled { labels.draw(text, at: point, in: ctx) }
        ctx.setShouldAntialias(!tuned)
        ctx.restoreGState()

        // Velocity lane.
        ctx.setFillColor(Theme.surface2)
        ctx.fill(CGRect(x: 0, y: grid.maxY, width: bounds.width, height: Self.velocityHeight))
        ctx.saveGState()
        ctx.clip(to: CGRect(x: grid.minX, y: grid.maxY, width: grid.width, height: Self.velocityHeight))
        ctx.addPath(stems)
        ctx.setStrokeColor(Theme.accent)
        ctx.setLineWidth(2)
        ctx.strokePath()
        ctx.restoreGState()
        ctx.setShouldAntialias(true)

        // Keyboard.
        ctx.setFillColor(Theme.surface3)
        ctx.fill(CGRect(x: 0, y: grid.minY, width: Self.keyboardWidth, height: grid.height))
        var blackKeys: [CGRect] = []
        let keySeparators = CGMutablePath()
        for r in 0...rowCount {
            let pitch = firstRow - r
            guard pitch >= 0 && pitch <= 127 else { continue }
            let y = rowY(pitch)
            if NoteNames.isBlack(pitch) {
                blackKeys.append(CGRect(x: 0, y: y, width: Self.keyboardWidth * 0.62, height: keyHeight))
            } else {
                keySeparators.move(to: CGPoint(x: 0, y: y + keyHeight))
                keySeparators.addLine(to: CGPoint(x: Self.keyboardWidth, y: y + keyHeight))
            }
            if pitch % 12 == 0 && keyHeight >= 8 {
                keyLabels.draw(NoteNames.name(pitch), at: CGPoint(x: Self.keyboardWidth - 26, y: y + keyHeight - 3), in: ctx)
            }
        }
        ctx.addPath(keySeparators)
        ctx.setStrokeColor(Theme.rgb(0, 0, 0, 0.25))
        ctx.setLineWidth(1)
        ctx.strokePath()
        ctx.setFillColor(Theme.blackKey)
        ctx.fill(blackKeys)

        // Ruler with bar numbers.
        ctx.setFillColor(Theme.surface2)
        ctx.fill(CGRect(x: 0, y: 0, width: bounds.width, height: Self.rulerHeight))
        var labelBars = 1
        while Double(labelBars) * barTicks / ticksPerPoint < 48 { labelBars *= 2 }
        var b = floor(originTick / barTicks / Double(labelBars)) * Double(labelBars)
        while b * barTicks <= endTick {
            let x = grid.minX + CGFloat((b * barTicks - originTick) / ticksPerPoint)
            if x >= grid.minX - 1 { rulerLabels.draw("\(Int(b) + 1)", at: CGPoint(x: x + 3, y: 16), in: ctx) }
            b += Double(labelBars)
        }

        meter.recordDraw(seconds: CACurrentMediaTime() - start, visibleItems: drawn)
    }

    private func rowY(_ pitch: Int) -> CGFloat {
        gridRect.minY + CGFloat(topPitch - Double(pitch)) * keyHeight
    }
}
