// Spike S3: rendering benchmark for the dense editors (docs/06 §3).
//
//   "JRB S3 Bench.app/Contents/MacOS/JRB S3 Bench" [--frames 600] [--output results.json] [--metal | --tuned] [--offscreen] [--canvas-scale 2]
//
// Opens a 1440x860 window, plays scripted scroll and zoom scenarios at the
// display's full refresh rate, prints a report and quits. The window must be
// visible and in front (display links pause for hidden windows).
// --offscreen renders into a Retina-sized bitmap instead: no window, drawing
// cost only, no display-pacing measurement.

import AppKit

let arguments = CommandLine.arguments
func argument(_ name: String) -> String? {
    guard let i = arguments.firstIndex(of: name), i + 1 < arguments.count else { return nil }
    return arguments[i + 1]
}

let controller = BenchController(outputPath: argument("--output"),
                                 framesPerScenario: Int(argument("--frames") ?? "") ?? 600,
                                 useMetal: arguments.contains("--metal"))
RenderOptions.tuned = arguments.contains("--tuned")
RenderOptions.canvasScale = CGFloat(Double(argument("--canvas-scale") ?? "") ?? 1)
let app = NSApplication.shared
if arguments.contains("--offscreen") {
    let ok = arguments.contains("--metal") ? controller.runMetalOffscreen() : controller.runOffscreen()
    exit(ok ? 0 : 1)
}
app.setActivationPolicy(.regular)
app.delegate = controller
app.run()
