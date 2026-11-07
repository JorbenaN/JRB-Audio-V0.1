import AppKit
import JRBCore

/// Core Graphics arrangement: track headers, ruler, clips with cached MIDI
/// previews, and audio waveforms drawn from a min/max peak pyramid.
@MainActor
final class ArrangementCanvas: NSView {
    let project: BenchProject
    let meter: FrameMeter
    let ppq: Double

    var originTick: Double = 0
    var ticksPerPoint: Double = 200
    var scrollY: CGFloat = 0          // content offset in points
    var trackHeight: CGFloat = 48

    static let headerWidth: CGFloat = 180
    static let rulerHeight: CGFloat = 24
    static let clipTitleHeight: CGFloat = 14

    private let titles = TextCache(font: Theme.smallFont, color: Theme.rgb(12, 14, 20, 0.9))
    private let names = TextCache(font: Theme.uiFont, color: Theme.textPrimary)
    private let rulerLabels = TextCache(font: Theme.uiFont, color: Theme.textSecondary)
    private var previews: [UInt64: CGImage] = [:]
    private let previewSize = (width: 512, height: 64)

    init(project: BenchProject, meter: FrameMeter) {
        self.project = project
        self.meter = meter
        self.ppq = Double(project.ppq)
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
        layer?.isOpaque = true
        buildPreviews()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }

    var lanesRect: CGRect {
        CGRect(x: Self.headerWidth, y: Self.rulerHeight, width: bounds.width - Self.headerWidth,
               height: bounds.height - Self.rulerHeight)
    }

    var contentHeight: CGFloat { CGFloat(project.rows.count) * trackHeight }

    /// One small bitmap per pattern, rendered once (re-rendered when the pattern
    /// changes, in the real app). Clips scale it instead of drawing every note.
    private func buildPreviews() {
        let snapshot = project.snapshot
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        for p in 0..<snapshot.patternCount() {
            let pattern = snapshot.patternAt(p)
            guard pattern.id != project.bigPattern,
                  let ctx = CGContext(data: nil, width: previewSize.width, height: previewSize.height, bitsPerComponent: 8,
                                      bytesPerRow: 0, space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { continue }
            let notes = snapshot.notes(pattern.id)
            var lo: Int32 = 127, hi: Int32 = 0
            for n in notes { lo = min(lo, n.pitch); hi = max(hi, n.pitch) }
            let range = CGFloat(max(12, hi - lo + 1))
            let scaleX = CGFloat(previewSize.width) / CGFloat(pattern.length)
            let rowH = CGFloat(previewSize.height) / range
            ctx.setFillColor(Theme.rgb(10, 12, 18, 0.75))
            var rects: [CGRect] = []
            rects.reserveCapacity(notes.count)
            for n in notes {
                rects.append(CGRect(x: CGFloat(n.start) * scaleX, y: CGFloat(n.pitch - lo) * rowH,
                                    width: max(1, CGFloat(n.length) * scaleX), height: max(1, rowH)))
            }
            ctx.fill(rects)
            if let image = ctx.makeImage() { previews[pattern.id] = image }
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        let start = CACurrentMediaTime()
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let lanes = lanesRect
        let endTick = originTick + Double(lanes.width) * ticksPerPoint
        let rows = project.rows
        let firstRow = max(0, Int(scrollY / trackHeight))
        let lastRow = min(rows.count - 1, Int((scrollY + lanes.height) / trackHeight))

        ctx.setFillColor(Theme.surface0)
        ctx.fill(bounds)

        // Alternating lane backgrounds.
        var alternate: [CGRect] = []
        if firstRow <= lastRow {
            for r in firstRow...lastRow where r % 2 == 1 {
                alternate.append(CGRect(x: lanes.minX, y: laneY(r), width: lanes.width, height: trackHeight))
            }
        }
        ctx.setFillColor(Theme.surface1)
        ctx.fill(alternate)

        // Bar grid.
        ctx.saveGState()
        ctx.clip(to: lanes)
        let barTicks = ppq * 4
        var stepBars = 1.0
        while stepBars * barTicks / ticksPerPoint < 12 { stepBars *= 2 }
        let gridPath = CGMutablePath()
        var b = floor(originTick / barTicks / stepBars) * stepBars
        while b * barTicks <= endTick {
            let x = lanes.minX + CGFloat((b * barTicks - originTick) / ticksPerPoint)
            gridPath.move(to: CGPoint(x: x, y: lanes.minY))
            gridPath.addLine(to: CGPoint(x: x, y: lanes.maxY))
            b += stepBars
        }
        let tuned = RenderOptions.tuned
        ctx.setShouldAntialias(!tuned)
        if tuned {
            let hair = 1 / (window?.backingScaleFactor ?? 2)
            var rects: [CGRect] = []
            gridPath.applyWithBlock { element in
                if element.pointee.type == .moveToPoint {
                    rects.append(CGRect(x: element.pointee.points[0].x, y: lanes.minY, width: hair, height: lanes.height))
                }
            }
            ctx.setFillColor(Theme.gridBeat.flattened(over: Theme.surface0))
            ctx.fill(rects)
        } else {
            ctx.addPath(gridPath)
            ctx.setStrokeColor(Theme.gridBeat)
            ctx.setLineWidth(1)
            ctx.strokePath()
        }

        // Clips: bodies batched per track colour, then content, then titles.
        var bodies = [CGMutablePath](repeating: CGMutablePath(), count: Theme.trackColors.count)
        for i in bodies.indices { bodies[i] = CGMutablePath() }
        var previewDraws: [(CGImage, CGRect, CGRect)] = []
        var waveformDraws: [(Waveform, CGRect, Double, Double)] = []   // clip rect, clip start tick, visible
        var titleDraws: [(String, CGPoint)] = []
        var visibleClips = 0
        let showContent = trackHeight >= 28

        if firstRow <= lastRow {
            for r in firstRow...lastRow {
                let row = rows[r]
                let y = laneY(r)
                func clipRect(_ startTick: Int64, _ length: Int64) -> CGRect? {
                    let s = Double(startTick), e = Double(startTick + length)
                    guard e > originTick && s < endTick else { return nil }
                    let x0 = lanes.minX + CGFloat((s - originTick) / ticksPerPoint)
                    let x1 = lanes.minX + CGFloat((e - originTick) / ticksPerPoint)
                    return CGRect(x: x0, y: y + 1, width: max(2, x1 - x0 - 1), height: trackHeight - 2)
                }
                for clip in row.clips {
                    guard let rect = clipRect(clip.start, clip.length) else { continue }
                    if tuned { bodies[row.colorIndex % bodies.count].addRect(rect) }
                    else { bodies[row.colorIndex % bodies.count].addRoundedRect(in: rect, cornerWidth: 3, cornerHeight: 3) }
                    visibleClips += 1
                    if showContent, let image = previews[clip.patternId] {
                        let content = CGRect(x: rect.minX + 2, y: rect.minY + Self.clipTitleHeight,
                                             width: rect.width - 4, height: rect.height - Self.clipTitleHeight - 2)
                        previewDraws.append((image, content, rect))
                    }
                    if rect.width > 48 && trackHeight >= 20 {
                        titleDraws.append((row.name, CGPoint(x: max(rect.minX, lanes.minX) + 4, y: rect.minY + 11)))
                    }
                }
                for clip in row.audioClips {
                    guard let rect = clipRect(clip.start, clip.length) else { continue }
                    if tuned { bodies[row.colorIndex % bodies.count].addRect(rect) }
                    else { bodies[row.colorIndex % bodies.count].addRoundedRect(in: rect, cornerWidth: 3, cornerHeight: 3) }
                    visibleClips += 1
                    waveformDraws.append((clip.waveform, rect, Double(clip.start), Double(clip.length)))
                    if rect.width > 48 && trackHeight >= 20 {
                        titleDraws.append((row.name, CGPoint(x: max(rect.minX, lanes.minX) + 4, y: rect.minY + 11)))
                    }
                }
            }
        }
        for (i, path) in bodies.enumerated() where !path.isEmpty {
            ctx.addPath(path)
            let color = Theme.trackColors[i].copy(alpha: 0.85)!
            ctx.setFillColor(tuned ? color.flattened(over: Theme.surface0) : color)
            ctx.fillPath()
        }
        ctx.interpolationQuality = tuned ? .none : .low
        for (image, content, clip) in previewDraws {
            ctx.saveGState()
            ctx.clip(to: clip.intersection(lanes))
            // Flip the image vertically into the flipped view.
            ctx.translateBy(x: content.minX, y: content.maxY)
            ctx.scaleBy(x: 1, y: -1)
            ctx.draw(image, in: CGRect(origin: .zero, size: content.size))
            ctx.restoreGState()
        }
        let waveformPath = CGMutablePath()
        for (waveform, rect, startTick, _) in waveformDraws {
            appendWaveform(waveform, clipRect: rect, clipStartTick: startTick, lanes: lanes, into: waveformPath)
        }
        ctx.addPath(waveformPath)
        ctx.setFillColor(Theme.rgb(10, 12, 18, 0.7))
        ctx.fillPath()
        ctx.setShouldAntialias(true)
        for (text, point) in titleDraws { titles.draw(text, at: point, in: ctx) }
        ctx.restoreGState()

        // Track headers.
        ctx.setFillColor(Theme.surface2)
        ctx.fill(CGRect(x: 0, y: lanes.minY, width: Self.headerWidth, height: lanes.height))
        var chips: [[CGRect]] = Array(repeating: [], count: Theme.trackColors.count)
        let separators = CGMutablePath()
        if firstRow <= lastRow {
            for r in firstRow...lastRow {
                let y = laneY(r)
                chips[rows[r].colorIndex % chips.count].append(CGRect(x: 0, y: y, width: 4, height: trackHeight))
                separators.move(to: CGPoint(x: 0, y: y + trackHeight))
                separators.addLine(to: CGPoint(x: Self.headerWidth, y: y + trackHeight))
                if trackHeight >= 18 {
                    names.draw(rows[r].name, at: CGPoint(x: 14, y: y + min(trackHeight / 2 + 4, 20)), in: ctx)
                }
            }
        }
        for (i, rects) in chips.enumerated() where !rects.isEmpty {
            ctx.setFillColor(Theme.trackColors[i])
            ctx.fill(rects)
        }
        ctx.addPath(separators)
        ctx.setStrokeColor(Theme.rgb(0, 0, 0, 0.3))
        ctx.strokePath()

        // Ruler.
        ctx.setFillColor(Theme.surface2)
        ctx.fill(CGRect(x: 0, y: 0, width: bounds.width, height: Self.rulerHeight))
        var labelBars = 1.0
        while labelBars * barTicks / ticksPerPoint < 40 { labelBars *= 2 }
        var lb = floor(originTick / barTicks / labelBars) * labelBars
        while lb * barTicks <= endTick {
            let x = lanes.minX + CGFloat((lb * barTicks - originTick) / ticksPerPoint)
            if x >= lanes.minX - 1 { rulerLabels.draw("\(Int(lb) + 1)", at: CGPoint(x: x + 3, y: 16), in: ctx) }
            lb += labelBars
        }

        meter.recordDraw(seconds: CACurrentMediaTime() - start, visibleItems: visibleClips)
    }

    private func laneY(_ row: Int) -> CGFloat {
        lanesRect.minY + CGFloat(row) * trackHeight - scrollY
    }

    /// Appends a closed min/max outline for the visible part of a clip, one
    /// column per device pixel, using the coarsest pyramid level that still has
    /// at least one peak per column.
    private func appendWaveform(_ waveform: Waveform, clipRect: CGRect, clipStartTick: Double, lanes: CGRect,
                                into path: CGMutablePath) {
        let body = CGRect(x: clipRect.minX, y: clipRect.minY + Self.clipTitleHeight, width: clipRect.width,
                          height: clipRect.height - Self.clipTitleHeight - 2)
        guard body.height > 6 else { return }
        let x0 = max(body.minX, lanes.minX), x1 = min(body.maxX, lanes.maxX)
        guard x1 > x0 else { return }
        let scale = window?.backingScaleFactor ?? 2
        let columnWidth = 1 / scale
        // 120 BPM: one quarter = 0.5 s. (The real app converts through the tempo map.)
        let secondsPerPoint = ticksPerPoint / ppq * 0.5
        let samplesPerColumn = secondsPerPoint * Double(columnWidth) * waveform.sampleRate
        var level = 0
        var samplesPerPeak = Double(Waveform.baseSamplesPerPeak)
        while level + 1 < waveform.levels.count && samplesPerPeak * 4 <= samplesPerColumn {
            level += 1
            samplesPerPeak *= 4
        }
        let peaks = waveform.levels[level]
        let mid = body.midY, half = body.height / 2
        var top: [CGPoint] = [], bottom: [CGPoint] = []
        let columns = Int((x1 - x0) / columnWidth)
        top.reserveCapacity(columns + 1)
        bottom.reserveCapacity(columns + 1)
        var x = x0
        while x <= x1 {
            let secondsIntoClip = (Double(x - body.minX)) * secondsPerPoint
            let first = Int(secondsIntoClip * waveform.sampleRate / samplesPerPeak)
            let last = max(first, Int((secondsIntoClip + secondsPerPoint * Double(columnWidth)) * waveform.sampleRate / samplesPerPeak))
            var lo: Float = 0, hi: Float = 0
            if first < peaks.count {
                for i in first...min(last, peaks.count - 1) {
                    lo = min(lo, peaks[i].0)
                    hi = max(hi, peaks[i].1)
                }
            }
            top.append(CGPoint(x: x, y: mid - CGFloat(hi) * half))
            bottom.append(CGPoint(x: x, y: mid - CGFloat(lo) * half))
            x += columnWidth
        }
        guard top.count > 1 else { return }
        path.addLines(between: top + bottom.reversed())
        path.closeSubpath()
    }
}
