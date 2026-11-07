import CoreGraphics
import simd

/// Backend-neutral drawing commands for the dense editors (the `Canvas`
/// abstraction of ADR-015). Editors build a DisplayList each frame from the
/// snapshot; a renderer (Metal here) turns it into pixels. Coordinates are in
/// points with a top-left origin. Items draw in the order they were added.
struct DisplayList {
    enum FontRole: UInt8 { case small, ui }

    struct Shape {
        var rect: CGRect
        var color: SIMD4<Float>    // straight (non-premultiplied) RGBA
        var radius: Float
        var clip: CGRect
    }

    /// An alpha mask from an atlas, tinted with a colour (pattern previews).
    struct Mask {
        var rect: CGRect
        var maskSlot: Int
        var color: SIMD4<Float>
        var clip: CGRect
    }

    struct Text {
        var string: String
        var role: FontRole
        var baseline: CGPoint
        var color: SIMD4<Float>
        var clip: CGRect
    }

    /// A whole audio clip body; the GPU shades each pixel column from peak data.
    struct Wave {
        var rect: CGRect
        var color: SIMD4<Float>
        var clip: CGRect
        var waveform: Int
        var level: Int
        var peakAtLeftEdge: Float     // peak index (at `level`) at rect.minX
        var peaksPerPoint: Float
    }

    enum Kind { case shapes, masks, texts, waves }
    struct Batch {
        var kind: Kind
        var range: Range<Int>
    }

    private(set) var shapes: [Shape] = []
    private(set) var masks: [Mask] = []
    private(set) var texts: [Text] = []
    private(set) var waves: [Wave] = []
    private(set) var batches: [Batch] = []

    static let noClip = CGRect(x: -1e6, y: -1e6, width: 2e6, height: 2e6)
    var clip: CGRect = DisplayList.noClip

    mutating func reset() {
        shapes.removeAll(keepingCapacity: true)
        masks.removeAll(keepingCapacity: true)
        texts.removeAll(keepingCapacity: true)
        waves.removeAll(keepingCapacity: true)
        batches.removeAll(keepingCapacity: true)
        clip = Self.noClip
    }

    mutating func rect(_ rect: CGRect, _ color: SIMD4<Float>, radius: Float = 0) {
        shapes.append(Shape(rect: rect, color: color, radius: radius, clip: clip))
        extend(.shapes, count: shapes.count)
    }

    mutating func mask(_ rect: CGRect, slot: Int, _ color: SIMD4<Float>) {
        masks.append(Mask(rect: rect, maskSlot: slot, color: color, clip: clip))
        extend(.masks, count: masks.count)
    }

    mutating func text(_ string: String, _ role: FontRole, at baseline: CGPoint, _ color: SIMD4<Float>) {
        texts.append(Text(string: string, role: role, baseline: baseline, color: color, clip: clip))
        extend(.texts, count: texts.count)
    }

    mutating func wave(_ item: Wave) {
        var item = item
        item.clip = clip
        waves.append(item)
        extend(.waves, count: waves.count)
    }

    var itemCount: Int { shapes.count + masks.count + texts.count + waves.count }

    private mutating func extend(_ kind: Kind, count: Int) {
        if let last = batches.last, last.kind == kind, last.range.upperBound == count - 1 {
            batches[batches.count - 1].range = last.range.lowerBound..<count
        } else {
            batches.append(Batch(kind: kind, range: (count - 1)..<count))
        }
    }
}

extension CGColor {
    var simd: SIMD4<Float> {
        let c = converted(to: CGColorSpace(name: CGColorSpace.sRGB)!, intent: .defaultIntent, options: nil)?.components ?? [0, 0, 0, 1]
        return SIMD4(Float(c[0]), Float(c[1]), Float(c.count > 2 ? c[2] : c[0]), Float(c.last ?? 1))
    }
}
