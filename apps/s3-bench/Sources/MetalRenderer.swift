import CoreText
import Metal
import QuartzCore

/// GPU renderer for DisplayList (S3 prototype of the Metal canvas, ADR-015).
///
/// Every item is one instanced quad. Shapes get anti-aliased rounded corners
/// from a signed-distance function in the fragment shader, so anti-aliasing
/// costs nothing extra. Text and pattern previews are alpha masks in atlases,
/// tinted per instance. Clipping is per instance. One draw call per batch.
@MainActor
final class MetalRenderer {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let shapePipeline: MTLRenderPipelineState
    private let maskPipeline: MTLRenderPipelineState
    private let wavePipeline: MTLRenderPipelineState
    private let peakBuffer: MTLBuffer
    private let levelTable: [[(offset: UInt32, count: UInt32)]]   // [waveform][level]
    private let sampler: MTLSamplerState
    private let textAtlas: TextAtlas
    private let previewAtlas: MTLTexture?
    private let previewColumns: Int

    // Triple-buffered instance storage.
    private var frameIndex = 0
    private var shapeBuffers: [MTLBuffer?] = [nil, nil, nil]
    private var previewBuffers: [MTLBuffer?] = [nil, nil, nil]
    private var textBuffers: [MTLBuffer?] = [nil, nil, nil]
    private var waveBuffers: [MTLBuffer?] = [nil, nil, nil]
    private let inFlight = DispatchSemaphore(value: 3)

    struct Uniforms { var viewSize: SIMD2<Float>; var scale: Float; var pad: Float = 0 }
    struct ShapeInstance { var rect: SIMD4<Float>; var color: SIMD4<Float>; var clip: SIMD4<Float>; var radius: Float; var pad = SIMD3<Float>() }
    struct MaskInstance { var rect: SIMD4<Float>; var uv: SIMD4<Float>; var color: SIMD4<Float>; var clip: SIMD4<Float> }
    struct WaveInstance { var rect: SIMD4<Float>; var clip: SIMD4<Float>; var color: SIMD4<Float>
                          var peakStart: Float; var peaksPerPoint: Float; var offset: UInt32; var count: UInt32 }

    init?(previews: PreviewMasks, waveforms: [Waveform], scale: CGFloat) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        do {
            // Compiled at launch for the spike; the app will ship a precompiled metallib.
            let library = try device.makeLibrary(source: Self.shaderSource, options: nil)
            func pipeline(_ vertex: String, _ fragment: String) throws -> MTLRenderPipelineState {
                let d = MTLRenderPipelineDescriptor()
                d.vertexFunction = library.makeFunction(name: vertex)
                d.fragmentFunction = library.makeFunction(name: fragment)
                let c = d.colorAttachments[0]!
                c.pixelFormat = .bgra8Unorm
                c.isBlendingEnabled = true
                c.sourceRGBBlendFactor = .one
                c.sourceAlphaBlendFactor = .one
                c.destinationRGBBlendFactor = .oneMinusSourceAlpha
                c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
                return try device.makeRenderPipelineState(descriptor: d)
            }
            shapePipeline = try pipeline("shapeVertex", "shapeFragment")
            maskPipeline = try pipeline("maskVertex", "maskFragment")
            wavePipeline = try pipeline("waveVertex", "waveFragment")
        } catch {
            print("Metal pipeline error: \(error)")
            return nil
        }
        // All peak pyramids in one buffer of (min, max) pairs, uploaded once.
        var flat: [SIMD2<Float>] = []
        var table: [[(offset: UInt32, count: UInt32)]] = []
        for w in waveforms {
            var levels: [(offset: UInt32, count: UInt32)] = []
            for level in w.levels {
                levels.append((UInt32(flat.count), UInt32(level.count)))
                flat.append(contentsOf: level.map { SIMD2($0.0, $0.1) })
            }
            table.append(levels)
        }
        guard let peaks = device.makeBuffer(bytes: flat, length: max(8, flat.count * MemoryLayout<SIMD2<Float>>.stride),
                                            options: .storageModeShared) else { return nil }
        peakBuffer = peaks
        levelTable = table

        let s = MTLSamplerDescriptor()
        s.minFilter = .linear
        s.magFilter = .linear
        sampler = device.makeSamplerState(descriptor: s)!
        textAtlas = TextAtlas(device: device, scale: scale)

        // Pack pattern previews into one R8 atlas.
        previewColumns = 8
        let rows = max(1, (previews.pixels.count + previewColumns - 1) / previewColumns)
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: PreviewMasks.width * previewColumns,
                                                          height: PreviewMasks.height * rows, mipmapped: false)
        td.usage = .shaderRead
        previewAtlas = device.makeTexture(descriptor: td)
        for (slot, bytes) in previews.pixels.enumerated() {
            let region = MTLRegionMake2D((slot % previewColumns) * PreviewMasks.width, (slot / previewColumns) * PreviewMasks.height,
                                         PreviewMasks.width, PreviewMasks.height)
            bytes.withUnsafeBytes { previewAtlas?.replace(region: region, mipmapLevel: 0, withBytes: $0.baseAddress!,
                                                          bytesPerRow: PreviewMasks.width) }
        }
    }

    private func buffer(_ buffers: inout [MTLBuffer?], bytes: Int) -> MTLBuffer {
        if let b = buffers[frameIndex], b.length >= bytes { return b }
        let b = device.makeBuffer(length: max(bytes * 2, 4096), options: .storageModeShared)!
        buffers[frameIndex] = b
        return b
    }

    struct FrameTiming { var encodeSeconds: Double; var commandBuffer: MTLCommandBuffer }

    /// Encodes the display list into `target` and commits. Returns CPU encode time.
    func render(_ dl: DisplayList, to target: MTLTexture, size: CGSize, scale: CGFloat,
                present drawable: CAMetalDrawable? = nil) -> FrameTiming {
        let start = CACurrentMediaTime()
        inFlight.wait()
        frameIndex = (frameIndex + 1) % 3

        func f4(_ r: CGRect) -> SIMD4<Float> { SIMD4(Float(r.minX), Float(r.minY), Float(r.width), Float(r.height)) }

        let shapeBuffer = buffer(&shapeBuffers, bytes: max(1, dl.shapes.count) * MemoryLayout<ShapeInstance>.stride)
        let shapes = shapeBuffer.contents().bindMemory(to: ShapeInstance.self, capacity: dl.shapes.count)
        for (i, s) in dl.shapes.enumerated() {
            shapes[i] = ShapeInstance(rect: f4(s.rect), color: s.color, clip: f4(s.clip), radius: s.radius)
        }

        let previewBuffer = buffer(&previewBuffers, bytes: max(1, dl.masks.count) * MemoryLayout<MaskInstance>.stride)
        let previewInstances = previewBuffer.contents().bindMemory(to: MaskInstance.self, capacity: dl.masks.count)
        let cellU = 1 / Float(previewColumns)
        let rowsCount = Float(previewAtlas.map { $0.height / PreviewMasks.height } ?? 1)
        for (i, m) in dl.masks.enumerated() {
            let uv = SIMD4(Float(m.maskSlot % previewColumns) * cellU, Float(m.maskSlot / previewColumns) / rowsCount, cellU, 1 / rowsCount)
            previewInstances[i] = MaskInstance(rect: f4(m.rect), uv: uv, color: m.color, clip: f4(m.clip))
        }

        let textBuffer = buffer(&textBuffers, bytes: max(1, dl.texts.count) * MemoryLayout<MaskInstance>.stride)
        let textInstances = textBuffer.contents().bindMemory(to: MaskInstance.self, capacity: dl.texts.count)
        for (i, t) in dl.texts.enumerated() {
            let glyph = textAtlas.entry(t.string, t.role)
            let rect = CGRect(x: t.baseline.x - glyph.inset, y: t.baseline.y - glyph.ascent - glyph.inset,
                              width: glyph.size.width, height: glyph.size.height)
            textInstances[i] = MaskInstance(rect: f4(rect), uv: glyph.uv, color: t.color, clip: f4(t.clip))
        }

        let waveBuffer = buffer(&waveBuffers, bytes: max(1, dl.waves.count) * MemoryLayout<WaveInstance>.stride)
        let waveInstances = waveBuffer.contents().bindMemory(to: WaveInstance.self, capacity: dl.waves.count)
        for (i, w) in dl.waves.enumerated() {
            let level = levelTable[w.waveform][w.level]
            waveInstances[i] = WaveInstance(rect: f4(w.rect), clip: f4(w.clip), color: w.color, peakStart: w.peakAtLeftEdge,
                                            peaksPerPoint: w.peaksPerPoint, offset: level.offset, count: level.count)
        }

        let commandBuffer = queue.makeCommandBuffer()!
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.09, green: 0.1, blue: 0.11, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass)!
        var uniforms = Uniforms(viewSize: SIMD2(Float(size.width), Float(size.height)), scale: Float(scale))
        encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
        encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
        encoder.setFragmentSamplerState(sampler, index: 0)
        for batch in dl.batches {
            switch batch.kind {
            case .shapes:
                encoder.setRenderPipelineState(shapePipeline)
                encoder.setVertexBuffer(shapeBuffer, offset: batch.range.lowerBound * MemoryLayout<ShapeInstance>.stride, index: 0)
            case .masks:
                encoder.setRenderPipelineState(maskPipeline)
                encoder.setVertexBuffer(previewBuffer, offset: batch.range.lowerBound * MemoryLayout<MaskInstance>.stride, index: 0)
                encoder.setFragmentTexture(previewAtlas, index: 0)
            case .texts:
                encoder.setRenderPipelineState(maskPipeline)
                encoder.setVertexBuffer(textBuffer, offset: batch.range.lowerBound * MemoryLayout<MaskInstance>.stride, index: 0)
                encoder.setFragmentTexture(textAtlas.texture, index: 0)
            case .waves:
                encoder.setRenderPipelineState(wavePipeline)
                encoder.setVertexBuffer(waveBuffer, offset: batch.range.lowerBound * MemoryLayout<WaveInstance>.stride, index: 0)
                encoder.setFragmentBuffer(peakBuffer, offset: 0, index: 0)
            }
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: batch.range.count)
        }
        encoder.endEncoding()
        if let drawable { commandBuffer.present(drawable) }
        let semaphore = inFlight
        commandBuffer.addCompletedHandler { _ in semaphore.signal() }
        commandBuffer.commit()
        return FrameTiming(encodeSeconds: CACurrentMediaTime() - start, commandBuffer: commandBuffer)
    }

    static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct Uniforms { float2 viewSize; float scale; float pad; };
    struct ShapeInstance { float4 rect; float4 color; float4 clip; float radius; float3 pad; };
    struct MaskInstance { float4 rect; float4 uv; float4 color; float4 clip; };

    struct ShapeOut { float4 position [[position]]; float2 local; float2 halfSize; float radius; float4 color; float4 clip; float2 pt; };
    struct MaskOut { float4 position [[position]]; float2 uv; float4 color; float4 clip; float2 pt; };

    static float4 toClip(float2 p, constant Uniforms& u) {
        return float4(p.x / u.viewSize.x * 2.0 - 1.0, 1.0 - p.y / u.viewSize.y * 2.0, 0.0, 1.0);
    }
    static bool outside(float2 p, float4 c) {
        return p.x < c.x || p.y < c.y || p.x > c.x + c.z || p.y > c.y + c.w;
    }

    vertex ShapeOut shapeVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                const device ShapeInstance* items [[buffer(0)]], constant Uniforms& u [[buffer(1)]]) {
        ShapeInstance s = items[iid];
        float2 corner = float2(vid & 1, vid >> 1);
        float pad = 1.0 / u.scale;                       // room for the anti-aliased edge
        float2 p = s.rect.xy - pad + corner * (s.rect.zw + 2.0 * pad);
        ShapeOut o;
        o.position = toClip(p, u);
        o.halfSize = s.rect.zw * 0.5;
        o.local = p - (s.rect.xy + o.halfSize);
        o.radius = s.radius;
        o.color = s.color;
        o.clip = s.clip;
        o.pt = p;
        return o;
    }

    fragment float4 shapeFragment(ShapeOut in [[stage_in]], constant Uniforms& u [[buffer(1)]]) {
        if (outside(in.pt, in.clip)) discard_fragment();
        float r = min(in.radius, min(in.halfSize.x, in.halfSize.y));
        float2 q = abs(in.local) - (in.halfSize - r);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;   // signed distance in points
        float coverage = clamp(0.5 - d * u.scale, 0.0, 1.0);
        float a = in.color.a * coverage;
        return float4(in.color.rgb * a, a);
    }

    vertex MaskOut maskVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                              const device MaskInstance* items [[buffer(0)]], constant Uniforms& u [[buffer(1)]]) {
        MaskInstance m = items[iid];
        float2 corner = float2(vid & 1, vid >> 1);
        float2 p = m.rect.xy + corner * m.rect.zw;
        MaskOut o;
        o.position = toClip(p, u);
        o.uv = m.uv.xy + corner * m.uv.zw;
        o.color = m.color;
        o.clip = m.clip;
        o.pt = p;
        return o;
    }

    struct WaveInstance { float4 rect; float4 clip; float4 color; float peakStart; float peaksPerPoint; uint offset; uint count; };
    struct WaveOut { float4 position [[position]]; float2 pt; float4 rect; float4 clip; float4 color;
                     float peakStart; float peaksPerPoint; uint offset [[flat]]; uint count [[flat]]; };

    vertex WaveOut waveVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                              const device WaveInstance* items [[buffer(0)]], constant Uniforms& u [[buffer(1)]]) {
        WaveInstance w = items[iid];
        float2 corner = float2(vid & 1, vid >> 1);
        float2 p = w.rect.xy + corner * w.rect.zw;
        WaveOut o;
        o.position = toClip(p, u);
        o.pt = p; o.rect = w.rect; o.clip = w.clip; o.color = w.color;
        o.peakStart = w.peakStart; o.peaksPerPoint = w.peaksPerPoint; o.offset = w.offset; o.count = w.count;
        return o;
    }

    // Each fragment reduces the few peaks under its device-pixel column to a
    // min/max span, then anti-aliases the span's top and bottom edges.
    fragment float4 waveFragment(WaveOut in [[stage_in]], constant Uniforms& u [[buffer(1)]],
                                 const device float2* peaks [[buffer(0)]]) {
        if (outside(in.pt, in.clip)) discard_fragment();
        float column = floor((in.pt.x - in.rect.x) * u.scale) / u.scale;
        float first = in.peakStart + column * in.peaksPerPoint;
        float last = first + in.peaksPerPoint / u.scale;
        int count = int(in.count);
        int i0 = max(0, int(floor(first)));
        if (i0 >= count) discard_fragment();
        int i1 = min(count - 1, max(i0, int(ceil(last)) - 1));
        i1 = min(i1, i0 + 63);
        float lo = 0.0, hi = 0.0;
        for (int i = i0; i <= i1; ++i) {
            float2 pk = peaks[in.offset + uint(i)];
            lo = min(lo, pk.x);
            hi = max(hi, pk.y);
        }
        float mid = in.rect.y + in.rect.w * 0.5;
        float halfHeight = in.rect.w * 0.5;
        float top = mid - hi * halfHeight - 0.5 / u.scale;
        float bottom = mid - lo * halfHeight + 0.5 / u.scale;
        float d = max(top - in.pt.y, in.pt.y - bottom);
        float coverage = clamp(0.5 - d * u.scale, 0.0, 1.0);
        float a = in.color.a * coverage;
        return float4(in.color.rgb * a, a);
    }

    fragment float4 maskFragment(MaskOut in [[stage_in]], texture2d<float> mask [[texture(0)]], sampler s [[sampler(0)]]) {
        if (outside(in.pt, in.clip)) discard_fragment();
        float a = in.color.a * mask.sample(s, in.uv).r;
        return float4(in.color.rgb * a, a);
    }
    """
}

/// Text labels rasterised once by Core Text into an R8 atlas (shelf packing).
@MainActor
final class TextAtlas {
    struct Entry { var uv: SIMD4<Float>; var size: CGSize; var ascent: CGFloat; var inset: CGFloat }
    let texture: MTLTexture
    private let scale: CGFloat
    private let side = 2048
    private var entries: [String: Entry] = [:]
    private var cursor = (x: 0, y: 0, rowHeight: 0)

    init(device: MTLDevice, scale: CGFloat) {
        self.scale = scale
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: side, height: side, mipmapped: false)
        d.usage = .shaderRead
        texture = device.makeTexture(descriptor: d)!
    }

    func entry(_ string: String, _ role: DisplayList.FontRole) -> Entry {
        let key = "\(role.rawValue)|\(string)"
        if let e = entries[key] { return e }
        let font = role == .small ? Theme.smallFont : Theme.uiFont
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1),
        ]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: string, attributes: attributes))
        var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
        let width = CGFloat(CTLineGetTypographicBounds(line, &ascent, &descent, &leading))
        let inset: CGFloat = 1
        let pw = Int(ceil((width + 2 * inset) * scale)), ph = Int(ceil((ascent + descent + 2 * inset) * scale))
        if cursor.x + pw > side { cursor = (0, cursor.y + cursor.rowHeight, 0) }
        guard cursor.y + ph <= side, pw > 0, ph > 0 else {
            // Atlas full: a real implementation would evict or grow. Not reached in the benchmark.
            return Entry(uv: .zero, size: .zero, ascent: 0, inset: 0)
        }
        var pixels = [UInt8](repeating: 0, count: pw * ph)
        pixels.withUnsafeMutableBytes { raw in
            if let ctx = CGContext(data: raw.baseAddress, width: pw, height: ph, bitsPerComponent: 8, bytesPerRow: pw,
                                   space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue) {
                ctx.scaleBy(x: scale, y: scale)
                ctx.textPosition = CGPoint(x: inset, y: descent + inset)
                CTLineDraw(line, ctx)
            }
        }
        // Bitmap-context memory is already top row first, matching the texture.
        pixels.withUnsafeBytes {
            texture.replace(region: MTLRegionMake2D(cursor.x, cursor.y, pw, ph), mipmapLevel: 0,
                            withBytes: $0.baseAddress!, bytesPerRow: pw)
        }
        let e = Entry(uv: SIMD4(Float(cursor.x) / Float(side), Float(cursor.y) / Float(side), Float(pw) / Float(side), Float(ph) / Float(side)),
                      size: CGSize(width: CGFloat(pw) / scale, height: CGFloat(ph) / scale), ascent: ascent, inset: inset)
        cursor = (cursor.x + pw + 1, cursor.y, max(cursor.rowHeight, ph + 1))
        entries[key] = e
        return e
    }
}
