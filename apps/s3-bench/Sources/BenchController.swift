import AppKit
import ImageIO
import Metal
import QuartzCore

/// Runs scripted scroll/zoom scenarios at the display's full refresh rate and
/// reports draw-time percentiles and dropped frames for each.
@MainActor
final class BenchController: NSObject, NSApplicationDelegate {
    struct Scenario {
        enum Editor { case pianoRoll, arrangement }
        let name: String
        let editor: Editor
        let update: (Double) -> Void   // progress 0...1 → viewport
    }

    private let outputPath: String?
    private let framesPerScenario: Int
    private let warmupFrames = 30
    private let meter = FrameMeter()
    private var window: NSWindow!
    private var project: BenchProject!
    private var pianoRoll: PianoRollCanvas!
    private var arrangement: ArrangementCanvas!
    private var scenarios: [Scenario] = []
    private var results: [FrameMeter.Result] = []
    private var current = 0
    private var frame = 0
    private var link: CADisplayLink?
    private var buildSeconds = 0.0
    // On-screen Metal mode.
    private let useMetal: Bool
    private var metalView: MetalHostView?
    private var renderer: MetalRenderer?
    private var previews: PreviewMasks?
    private var displayList = DisplayList()

    init(outputPath: String?, framesPerScenario: Int, useMetal: Bool = false) {
        self.outputPath = outputPath
        self.framesPerScenario = framesPerScenario
        self.useMetal = useMetal
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let t0 = CACurrentMediaTime()
        project = BenchProject()
        buildSeconds = CACurrentMediaTime() - t0

        let frame = NSRect(x: 0, y: 0, width: 1440, height: 860)
        window = NSWindow(contentRect: frame, styleMask: [.titled, .closable, .resizable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "JRB Audio · S3 rendering benchmark"
        window.center()
        pianoRoll = PianoRollCanvas(snapshot: project.snapshot, patternId: project.bigPattern, ppq: project.ppq, meter: meter)
        arrangement = ArrangementCanvas(project: project, meter: meter)
        scenarios = makeScenarios()
        if useMetal {
            let masks = PreviewMasks(project: project)
            guard let r = MetalRenderer(previews: masks, waveforms: project.waveforms, scale: window.backingScaleFactor) else {
                print("Metal is not available.")
                NSApp.terminate(nil)
                return
            }
            previews = masks
            renderer = r
            let host = MetalHostView(device: r.device)
            metalView = host
            window.contentView = host
            window.title += " · Metal"
        } else {
            show(scenarios[0].editor)
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate()

        let link = window.contentView!.displayLink(target: self, selector: #selector(tick(_:)))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 120, maximum: 120, preferred: 120)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    private func show(_ editor: Scenario.Editor) {
        guard !useMetal else { return }
        let view: NSView = editor == .pianoRoll ? pianoRoll : arrangement
        if window.contentView !== view {
            window.contentView = view
        }
    }

    @objc private func tick(_ link: CADisplayLink) {
        let scenario = scenarios[current]
        if frame == 0 { show(scenario.editor) }
        if frame == warmupFrames { meter.reset() }
        if frame >= warmupFrames { meter.recordTick(link.timestamp) }
        let progress = max(0, Double(frame - warmupFrames) / Double(framesPerScenario))
        scenario.update(min(1, progress))
        if let host = metalView, let renderer, let previews {
            // Build and encode on the main thread, as the app will; present on the next vsync.
            let start = CACurrentMediaTime()
            let size = host.bounds.size
            let visible = scenario.editor == .pianoRoll
                ? pianoRoll.build(&displayList, size: size, scale: host.scale)
                : arrangement.build(&displayList, size: size, scale: host.scale, previews: previews)
            if let drawable = host.metalLayer.nextDrawable() {
                _ = renderer.render(displayList, to: drawable.texture, size: size, scale: host.scale, present: drawable)
            }
            meter.recordDraw(seconds: CACurrentMediaTime() - start, visibleItems: visible)
        } else {
            window.contentView?.needsDisplay = true
        }
        frame += 1

        if frame > warmupFrames + framesPerScenario {
            let hz = Double(window.screen?.maximumFramesPerSecond ?? 60)
            results.append(meter.result(scenario: scenario.name, refreshHz: hz))
            current += 1
            frame = 0
            if current == scenarios.count { finish() }
        }
    }

    private func finish() {
        link?.invalidate()
        link = nil
        let hz = Double(window.screen?.maximumFramesPerSecond ?? 60)
        let ok = report(hz: hz, scale: window.backingScaleFactor,
                        mode: String(format: "on screen (%@): %@, %.0f Hz", useMetal ? "Metal" : "Core Graphics",
                                     window.screen?.localizedName ?? "?", hz))
        fflush(stdout)
        _ = ok
        NSApp.terminate(nil)
    }

    /// Offscreen mode: renders every frame of every scenario into a Retina-sized
    /// bitmap (the same Core Graphics rasterizer a layer-backed view uses) without
    /// opening a window. Measures drawing cost only, not display pacing.
    func runOffscreen() -> Bool {
        let t0 = CACurrentMediaTime()
        project = BenchProject()
        buildSeconds = CACurrentMediaTime() - t0
        let size = NSSize(width: 1440 * RenderOptions.canvasScale, height: 860 * RenderOptions.canvasScale)
        let scale: CGFloat = 2
        pianoRoll = PianoRollCanvas(snapshot: project.snapshot, patternId: project.bigPattern, ppq: project.ppq, meter: meter)
        arrangement = ArrangementCanvas(project: project, meter: meter)
        pianoRoll.frame = NSRect(origin: .zero, size: size)
        arrangement.frame = NSRect(origin: .zero, size: size)
        scenarios = makeScenarios()

        let width = Int(size.width * scale), height = Int(size.height * scale)
        guard let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return false }
        let graphics = NSGraphicsContext(cgContext: ctx, flipped: true)
        for scenario in scenarios {
            let view: NSView = scenario.editor == .pianoRoll ? pianoRoll : arrangement
            for f in 0..<(warmupFrames + framesPerScenario) {
                if f == warmupFrames { meter.reset() }
                scenario.update(min(1, max(0, Double(f - warmupFrames) / Double(framesPerScenario))))
                ctx.saveGState()
                ctx.translateBy(x: 0, y: CGFloat(height))
                ctx.scaleBy(x: scale, y: -scale)
                NSGraphicsContext.current = graphics
                view.draw(view.bounds)
                NSGraphicsContext.current = nil
                ctx.restoreGState()
                if f == warmupFrames + framesPerScenario / 2, let outputPath, let image = ctx.makeImage() {
                    let url = URL(fileURLWithPath: URL(fileURLWithPath: outputPath).deletingPathExtension().path
                                  + "-cg-\(scenarios.firstIndex { $0.name == scenario.name }! + 1).png")
                    if let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) {
                        CGImageDestinationAddImage(dest, image, nil)
                        CGImageDestinationFinalize(dest)
                    }
                }
            }
            results.append(meter.result(scenario: scenario.name, refreshHz: 120))
        }
        // Keep the last frame for visual inspection.
        if let outputPath, let image = ctx.makeImage() {
            let url = URL(fileURLWithPath: outputPath).deletingPathExtension().appendingPathExtension("png")
            if let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) {
                CGImageDestinationAddImage(dest, image, nil)
                CGImageDestinationFinalize(dest)
            }
        }
        return report(hz: 120, scale: scale, mode: String(format: "offscreen %.0f x %.0f pt bitmap, %@ Core Graphics, 120 Hz target (drawing cost only)",
                                                         size.width, size.height, RenderOptions.tuned ? "tuned" : "default"))
    }

    /// Metal mode: build the display list on the CPU, render it on the GPU into a
    /// Retina-sized texture, and measure CPU build, CPU encode and GPU time separately.
    func runMetalOffscreen() -> Bool {
        let t0 = CACurrentMediaTime()
        project = BenchProject()
        buildSeconds = CACurrentMediaTime() - t0
        let size = CGSize(width: 1440 * RenderOptions.canvasScale, height: 860 * RenderOptions.canvasScale)
        let scale: CGFloat = 2
        pianoRoll = PianoRollCanvas(snapshot: project.snapshot, patternId: project.bigPattern, ppq: project.ppq, meter: meter)
        arrangement = ArrangementCanvas(project: project, meter: meter)
        scenarios = makeScenarios()
        let previews = PreviewMasks(project: project)
        guard let renderer = MetalRenderer(previews: previews, waveforms: project.waveforms, scale: scale) else {
            print("Metal is not available.")
            return false
        }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: Int(size.width * scale),
                                                          height: Int(size.height * scale), mipmapped: false)
        td.usage = .renderTarget
        td.storageMode = .shared
        let target = renderer.device.makeTexture(descriptor: td)!
        var dl = DisplayList()

        print(String(format: "Mode: Metal, offscreen %.0f x %.0f pt @2x (%d x %d px), GPU: %@. Frame budget 8.33 ms",
                     size.width, size.height, target.width, target.height, renderer.device.name))
        print("Scenario                                    items  quads |  CPU p50   p95   p99   max |  GPU p50   p95   p99   max")
        var allPass = true
        var metalResults: [[String: Double]] = []
        for (index, scenario) in scenarios.enumerated() {
            var cpu: [Double] = [], gpu: [Double] = [], items: [Int] = [], quads: [Int] = []
            for f in 0..<(warmupFrames + framesPerScenario) {
                scenario.update(min(1, max(0, Double(f - warmupFrames) / Double(framesPerScenario))))
                let start = CACurrentMediaTime()
                let visible = scenario.editor == .pianoRoll
                    ? pianoRoll.build(&dl, size: size, scale: scale)
                    : arrangement.build(&dl, size: size, scale: scale, previews: previews)
                let frame = renderer.render(dl, to: target, size: size, scale: scale)
                let cpuSeconds = CACurrentMediaTime() - start
                frame.commandBuffer.waitUntilCompleted()
                if f >= warmupFrames {
                    cpu.append(cpuSeconds * 1000)
                    gpu.append((frame.commandBuffer.gpuEndTime - frame.commandBuffer.gpuStartTime) * 1000)
                    items.append(visible)
                    quads.append(dl.itemCount)
                }
                if f == warmupFrames + framesPerScenario / 2, let outputPath {
                    savePNG(target, to: URL(fileURLWithPath: outputPath).deletingPathExtension().path + "-metal-\(index + 1).png")
                }
            }
            func stats(_ v: [Double]) -> (Double, Double, Double, Double) {
                let s = v.sorted()
                func p(_ q: Double) -> Double { s[min(s.count - 1, Int(Double(s.count) * q))] }
                return (p(0.5), p(0.95), p(0.99), s.last ?? 0)
            }
            let c = stats(cpu), g = stats(gpu)
            let pass = c.2 < 4.17 && g.2 < 4.17
            allPass = allPass && pass
            let avgItems = Double(items.reduce(0, +)) / Double(max(1, items.count))
            let avgQuads = Double(quads.reduce(0, +)) / Double(max(1, quads.count))
            print(String(format: "%-42@ %6.0f %6.0f | %6.2f %5.2f %5.2f %5.2f | %6.2f %5.2f %5.2f %5.2f  %@",
                         scenario.name as NSString, avgItems, avgQuads, c.0, c.1, c.2, c.3, g.0, g.1, g.2, g.3, pass ? "PASS" : "FAIL"))
            metalResults.append(["cpuP50": c.0, "cpuP99": c.2, "gpuP50": g.0, "gpuP99": g.2, "quads": avgQuads])
        }
        print("\nCriteria: CPU (build + encode) p99 < 4.17 ms and GPU p99 < 4.17 ms (each half a 120 Hz frame)")
        print("S3 rendering (Metal): \(allPass ? "PASS" : "FAIL")")
        if let outputPath, let data = try? JSONSerialization.data(withJSONObject: metalResults, options: [.prettyPrinted]) {
            try? data.write(to: URL(fileURLWithPath: outputPath))
        }
        return allPass
    }

    private func savePNG(_ texture: MTLTexture, to path: String) {
        let bytesPerRow = texture.width * 4
        var bytes = [UInt8](repeating: 0, count: bytesPerRow * texture.height)
        texture.getBytes(&bytes, bytesPerRow: bytesPerRow, from: MTLRegionMake2D(0, 0, texture.width, texture.height), mipmapLevel: 0)
        bytes.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(data: raw.baseAddress, width: texture.width, height: texture.height, bitsPerComponent: 8,
                                      bytesPerRow: bytesPerRow, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                  let image = ctx.makeImage(),
                  let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)
            else { return }
            CGImageDestinationAddImage(dest, image, nil)
            CGImageDestinationFinalize(dest)
        }
    }

    @discardableResult
    private func report(hz: Double, scale: CGFloat, mode: String) -> Bool {
        let budget = 1000 / hz
        print(String(format: "Mode: %@. Frame budget %.2f ms, @%.0fx", mode, budget, scale))
        print(String(format: "Project: 200 tracks (40 audio), %d MIDI clips, 20,000-note piano-roll pattern; built in %.0f ms",
                     project.rows.reduce(0) { $0 + $1.clips.count }, buildSeconds * 1000))
        print("")
        print("Scenario                                    items   draw p50   p95    p99    max  | dropped  delivered")
        var allPass = true
        for r in results {
            let onScreen = r.deliveredPercent >= 0
            let pass = r.drawP99Ms < budget / 2 && (!onScreen || r.deliveredPercent >= 99)
            allPass = allPass && pass
            let pacing = onScreen ? String(format: "%5d   %6.1f%%", r.hitches, r.deliveredPercent) : "    -        -  "
            print(String(format: "%-42@ %6.0f  %6.2f %6.2f %6.2f %6.2f  | %@  %@",
                         r.scenario as NSString, r.avgVisibleItems, r.drawP50Ms, r.drawP95Ms, r.drawP99Ms, r.drawMaxMs,
                         pacing as NSString, pass ? "PASS" : "FAIL"))
        }
        print(String(format: "\nCriteria: p99 draw < %.2f ms (half a frame)%@", budget / 2,
                     results.first.map { $0.deliveredPercent >= 0 } == true ? " and >= 99% of frames delivered" : ""))
        print("S3 rendering: \(allPass ? "PASS" : "FAIL")")
        if let outputPath {
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            if let data = try? encoder.encode(results) {
                try? data.write(to: URL(fileURLWithPath: outputPath))
            }
        }
        return allPass
    }

    private func makeScenarios() -> [Scenario] {
        let ppq = Double(project.ppq)
        let bar = ppq * 4
        let pr = pianoRoll!, arr = arrangement!
        let canvas = CGSize(width: 1440 * RenderOptions.canvasScale, height: 860 * RenderOptions.canvasScale)
        let prWidth = canvas.width - PianoRollCanvas.keyboardWidth
        let arrWidth = canvas.width - ArrangementCanvas.headerWidth
        let lanesHeight: CGFloat = canvas.height - ArrangementCanvas.rulerHeight
        func triangle(_ p: Double) -> Double { p < 0.5 ? p * 2 : (1 - p) * 2 }

        return [
            Scenario(name: "Piano roll: scroll, 16 bars visible", editor: .pianoRoll) { p in
                pr.keyHeight = 14
                pr.ticksPerPoint = 16 * bar / Double(prWidth)
                pr.originTick = p * 80 * bar
                pr.topPitch = 92 + 6 * sin(p * 2 * .pi)
            },
            Scenario(name: "Piano roll: scroll, 128 bars (dense)", editor: .pianoRoll) { p in
                pr.keyHeight = 8
                pr.ticksPerPoint = 128 * bar / Double(prWidth)
                pr.originTick = p * 400 * bar
                pr.topPitch = 100
            },
            Scenario(name: "Piano roll: zoom 2 to 256 bars and back", editor: .pianoRoll) { p in
                let t = triangle(p)
                let bars = 2 * pow(128, t)
                pr.keyHeight = CGFloat(20 - 12 * t)
                pr.ticksPerPoint = bars * bar / Double(prWidth)
                pr.originTick = 100 * bar
                pr.topPitch = 94
            },
            Scenario(name: "Arrangement: scroll, 32 bars x 17 tracks", editor: .arrangement) { p in
                arr.trackHeight = 48
                arr.ticksPerPoint = 32 * bar / Double(arrWidth)
                arr.originTick = p * 300 * bar
                arr.scrollY = CGFloat(p) * (arr.contentHeight - lanesHeight)
            },
            Scenario(name: "Arrangement: zoom 8 bars to whole song", editor: .arrangement) { p in
                let t = triangle(p)
                let songBars = Double(self.project.songLength) / bar
                let bars = 8 * pow(songBars / 8, t)
                arr.trackHeight = 32
                arr.ticksPerPoint = bars * bar / Double(arrWidth)
                arr.originTick = 0
                arr.scrollY = 3000
            },
            Scenario(name: "Arrangement: audio waveforms, 16 bars", editor: .arrangement) { p in
                arr.trackHeight = 64
                arr.ticksPerPoint = 16 * bar / Double(arrWidth)
                arr.originTick = p * 200 * bar
                arr.scrollY = arr.contentHeight - lanesHeight
            },
            Scenario(name: "Arrangement: whole song, 20-pt tracks", editor: .arrangement) { p in
                arr.trackHeight = 20
                arr.ticksPerPoint = Double(self.project.songLength) / Double(arrWidth)
                arr.originTick = 0
                arr.scrollY = CGFloat(p) * (arr.contentHeight - lanesHeight)
            },
        ]
    }
}
