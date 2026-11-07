import Foundation
import QuartzCore

/// Collects per-frame timings for one scenario.
///  - draw time: CPU time inside draw(_:) (what our code costs)
///  - frame interval: time between display-link callbacks (what the user sees);
///    an interval longer than 1.5 refresh periods is a dropped frame (hitch).
@MainActor
final class FrameMeter {
    struct Result: Codable {
        let scenario: String
        let frames: Int
        let refreshHz: Double
        let drawP50Ms: Double
        let drawP95Ms: Double
        let drawP99Ms: Double
        let drawMaxMs: Double
        let hitches: Int
        let longestIntervalMs: Double
        let deliveredPercent: Double   // -1 when not measured (offscreen)
        let avgVisibleItems: Double
    }

    private var drawTimes: [Double] = []
    private var intervals: [Double] = []
    private var lastTimestamp: CFTimeInterval?
    private var itemCounts: [Int] = []
    private var firstTimestamp: CFTimeInterval = 0
    private var latestTimestamp: CFTimeInterval = 0

    func reset() {
        drawTimes.removeAll(keepingCapacity: true)
        intervals.removeAll(keepingCapacity: true)
        itemCounts.removeAll(keepingCapacity: true)
        lastTimestamp = nil
    }

    func recordTick(_ timestamp: CFTimeInterval) {
        if let last = lastTimestamp {
            intervals.append(timestamp - last)
        } else {
            firstTimestamp = timestamp
        }
        lastTimestamp = timestamp
        latestTimestamp = timestamp
    }

    func recordDraw(seconds: Double, visibleItems: Int) {
        drawTimes.append(seconds * 1000)
        itemCounts.append(visibleItems)
    }

    func result(scenario: String, refreshHz: Double) -> Result {
        let sorted = drawTimes.sorted()
        func pct(_ p: Double) -> Double { sorted.isEmpty ? 0 : sorted[min(sorted.count - 1, Int(Double(sorted.count) * p))] }
        let period = 1.0 / refreshHz
        let hitches = intervals.filter { $0 > period * 1.5 }.count
        let elapsed = latestTimestamp - firstTimestamp
        let expected = max(1.0, elapsed / period)
        return Result(scenario: scenario, frames: drawTimes.count, refreshHz: refreshHz,
                      drawP50Ms: pct(0.5), drawP95Ms: pct(0.95), drawP99Ms: pct(0.99), drawMaxMs: sorted.last ?? 0,
                      hitches: hitches, longestIntervalMs: (intervals.max() ?? 0) * 1000,
                      deliveredPercent: intervals.isEmpty ? -1 : min(100, 100 * Double(intervals.count) / expected),
                      avgVisibleItems: itemCounts.isEmpty ? 0 : Double(itemCounts.reduce(0, +)) / Double(itemCounts.count))
    }
}
