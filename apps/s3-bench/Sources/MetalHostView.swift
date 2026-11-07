import AppKit
import QuartzCore

/// An NSView backed by a CAMetalLayer: the on-screen home of the Metal canvas.
/// Frames are driven by the window's display link; each tick builds a display
/// list and presents one drawable.
@MainActor
final class MetalHostView: NSView {
    let metalLayer = CAMetalLayer()

    init(device: MTLDevice) {
        super.init(frame: .zero)
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.maximumDrawableCount = 3
        metalLayer.displaySyncEnabled = true
        wantsLayer = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    override func makeBackingLayer() -> CALayer { metalLayer }
    override var isOpaque: Bool { true }
    override var isFlipped: Bool { true }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    var scale: CGFloat { window?.backingScaleFactor ?? 2 }

    private func updateDrawableSize() {
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
    }
}
