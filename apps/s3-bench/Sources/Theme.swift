import AppKit
import CoreText

/// Early design tokens (docs/01 §7): graphite surfaces, one accent, a 12-hue
/// track palette of similar perceived lightness. The real DesignTokens module
/// arrives in Phase 2.
@MainActor
enum Theme {
    static func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
        CGColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: a)
    }

    static let surface0 = rgb(24, 25, 28)       // editor background
    static let surface1 = rgb(30, 31, 35)       // alternate rows / white-key rows
    static let surface2 = rgb(38, 40, 45)       // headers
    static let surface3 = rgb(52, 54, 60)       // keyboard white keys
    static let blackKey = rgb(18, 18, 21)
    static let gridMinor = rgb(255, 255, 255, 0.045)
    static let gridBeat = rgb(255, 255, 255, 0.08)
    static let gridBar = rgb(255, 255, 255, 0.16)
    static let textPrimary = rgb(232, 233, 237)
    static let textSecondary = rgb(150, 153, 162)
    static let accent = rgb(90, 160, 255)

    /// Track colors: hues spaced 30° apart at similar lightness/chroma.
    static let trackColors: [CGColor] = [
        rgb(232, 107, 107), rgb(230, 142, 84), rgb(222, 180, 74), rgb(180, 196, 80),
        rgb(116, 196, 108), rgb(80, 192, 152), rgb(72, 184, 200), rgb(88, 158, 232),
        rgb(124, 132, 238), rgb(164, 118, 230), rgb(208, 108, 202), rgb(228, 104, 156),
    ]

    static func trackColor(_ index: Int) -> CGColor { trackColors[index % trackColors.count] }

    /// Note fill by velocity bucket (0...7): darker = softer.
    static let velocityColors: [CGColor] = (0..<8).map { i in
        let t = CGFloat(i) / 7
        return rgb(70 + 40 * t, 110 + 60 * t, 170 + 85 * t)
    }

    static let uiFont = CTFontCreateUIFontForLanguage(.system, 11, nil)!
    static let smallFont = CTFontCreateUIFontForLanguage(.system, 9.5, nil)!
}

/// Benchmark switches (S3): the "tuned" Core Graphics path trades rounded,
/// anti-aliased and translucent shapes for opaque, pixel-snapped rectangles.
@MainActor
enum RenderOptions {
    static var tuned = false
    static var canvasScale: CGFloat = 1   // multiplies the canvas size (pixel-scaling test)
}

extension CGColor {
    /// The opaque colour this translucent colour produces over `background`.
    func flattened(over background: CGColor) -> CGColor {
        guard let c = converted(to: CGColorSpace(name: CGColorSpace.sRGB)!, intent: .defaultIntent, options: nil)?.components,
              let b = background.converted(to: CGColorSpace(name: CGColorSpace.sRGB)!, intent: .defaultIntent, options: nil)?.components,
              c.count == 4, b.count >= 3 else { return self }
        let a = c[3]
        return CGColor(srgbRed: c[0] * a + b[0] * (1 - a), green: c[1] * a + b[1] * (1 - a), blue: c[2] * a + b[2] * (1 - a), alpha: 1)
    }
}

/// Caches laid-out text lines; text shaping is the most expensive thing Core
/// Graphics editors draw, so every label is shaped once and reused.
@MainActor
final class TextCache {
    private var lines: [String: CTLine] = [:]
    private let font: CTFont
    private let color: CGColor

    init(font: CTFont, color: CGColor) {
        self.font = font
        self.color = color
    }

    func line(_ text: String) -> CTLine {
        if let cached = lines[text] { return cached }
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): color,
        ]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes))
        lines[text] = line
        return line
    }

    /// Draws at a baseline in a flipped (top-left origin) context.
    func draw(_ text: String, at point: CGPoint, in ctx: CGContext) {
        ctx.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
        ctx.textPosition = point
        CTLineDraw(line(text), ctx)
    }
}

enum NoteNames {
    static let names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"]
    static func name(_ pitch: Int) -> String { "\(names[pitch % 12])\(pitch / 12 - 2)" }
    static func isBlack(_ pitch: Int) -> Bool { [1, 3, 6, 8, 10].contains(pitch % 12) }
}
