/*
 * MetalRenderer.swift
 * Metal-based NV12 → RGB renderer using CVMetalTextureCache for zero-copy.
 *
 * Architecture:
 *   - Owns a custom MTKView (VideoView) that accepts first responder.
 *   - Uses CVMetalTextureCache to create Metal textures directly from
 *     IOSurface-backed CVPixelBuffers (zero-copy from VideoToolbox).
 *   - BT.709 YCbCr→RGB conversion in a fragment shader.
 *   - Full-screen triangle (3 vertices, no index buffer) for the quad.
 *   - isPaused=true, enableSetNeedsDisplay=true (demand-driven rendering).
 */

import Foundation
import Metal
import MetalKit
import CoreVideo

// MARK: - VideoView (MTKView subclass that accepts first responder)

class VideoView: MTKView {
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

// MARK: - MetalRenderer

final class MetalRenderer: NSObject, MTKViewDelegate {

    let device: MTLDevice
    let view: VideoView
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState
    private var textureCache: CVMetalTextureCache?
    private var currentPixelBuffer: CVPixelBuffer?

    // MARK: - Init

    init?() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            print("[renderer] ERROR: no Metal device")
            return nil
        }
        self.device = device

        guard let cq = device.makeCommandQueue() else {
            print("[renderer] ERROR: failed to create command queue")
            return nil
        }
        self.commandQueue = cq

        // Create texture cache for zero-copy CVPixelBuffer → MTLTexture
        var cache: CVMetalTextureCache?
        let cacheStatus = CVMetalTextureCacheCreate(
            kCFAllocatorDefault, nil, device, nil, &cache
        )
        guard cacheStatus == kCVReturnSuccess, let tc = cache else {
            print("[renderer] ERROR: CVMetalTextureCacheCreate failed (\(cacheStatus))")
            return nil
        }
        self.textureCache = tc

        // Create MTKView
        let mtkView = VideoView(frame: .zero, device: device)
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.framebufferOnly = false
        mtkView.isPaused = true
        mtkView.enableSetNeedsDisplay = true
        mtkView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        self.view = mtkView

        // Compile Metal shaders
        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: MetalRenderer.shaderSource, options: nil)
        } catch {
            print("[renderer] ERROR: shader compilation failed: \(error)")
            return nil
        }

        guard let vertexFunc = library.makeFunction(name: "vertexShader"),
              let fragmentFunc = library.makeFunction(name: "fragmentShader") else {
            print("[renderer] ERROR: shader functions not found")
            return nil
        }

        let pipelineDesc = MTLRenderPipelineDescriptor()
        pipelineDesc.vertexFunction = vertexFunc
        pipelineDesc.fragmentFunction = fragmentFunc
        pipelineDesc.colorAttachments[0].pixelFormat = .bgra8Unorm

        do {
            self.pipelineState = try device.makeRenderPipelineState(descriptor: pipelineDesc)
        } catch {
            print("[renderer] ERROR: pipeline state creation failed: \(error)")
            return nil
        }

        super.init()
        mtkView.delegate = self
        print("[renderer] initialized (device: \(device.name))")
    }

    // MARK: - Public API

    /// Enqueue a decoded pixel buffer for display. Must be called on main thread.
    func display(pixelBuffer: CVPixelBuffer) {
        currentPixelBuffer = pixelBuffer
        view.setNeedsDisplay(view.bounds)
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // Nothing to do — shader handles any resolution
    }

    func draw(in view: MTKView) {
        guard let pixelBuffer = currentPixelBuffer else { return }
        guard let textureCache = textureCache else { return }
        guard let drawable = view.currentDrawable else { return }
        guard let rpd = view.currentRenderPassDescriptor else { return }

        // Create Metal textures from CVPixelBuffer planes (zero-copy via IOSurface)
        let yWidth  = CVPixelBufferGetWidthOfPlane(pixelBuffer, 0)
        let yHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0)
        var yTextureRef: CVMetalTexture?
        let yStatus = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pixelBuffer, nil,
            .r8Unorm, yWidth, yHeight, 0, &yTextureRef
        )

        let uvWidth  = CVPixelBufferGetWidthOfPlane(pixelBuffer, 1)
        let uvHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 1)
        var uvTextureRef: CVMetalTexture?
        let uvStatus = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pixelBuffer, nil,
            .rg8Unorm, uvWidth, uvHeight, 1, &uvTextureRef
        )

        guard yStatus == kCVReturnSuccess,
              uvStatus == kCVReturnSuccess,
              let yRef = yTextureRef,
              let uvRef = uvTextureRef,
              let yTexture = CVMetalTextureGetTexture(yRef),
              let uvTexture = CVMetalTextureGetTexture(uvRef) else {
            // Fallback: manual texture upload if IOSurface not available
            drawManualUpload(pixelBuffer: pixelBuffer, drawable: drawable, rpd: rpd)
            return
        }

        // Render
        renderFrame(yTexture: yTexture, uvTexture: uvTexture,
                    drawable: drawable, rpd: rpd)
    }

    // MARK: - Render

    private func renderFrame(yTexture: MTLTexture, uvTexture: MTLTexture,
                             drawable: CAMetalDrawable,
                             rpd: MTLRenderPassDescriptor) {
        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: rpd) else {
            return
        }

        encoder.setRenderPipelineState(pipelineState)
        encoder.setFragmentTexture(yTexture, index: 0)
        encoder.setFragmentTexture(uvTexture, index: 1)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()
    }

    // MARK: - Manual Upload Fallback

    /// Fallback for non-IOSurface pixel buffers: lock, copy to textures, render.
    private func drawManualUpload(pixelBuffer: CVPixelBuffer,
                                  drawable: CAMetalDrawable,
                                  rpd: MTLRenderPassDescriptor) {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

        let yWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 0)
        let yHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0)
        let yBytesPerRow = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0)
        guard let yBase = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0) else { return }

        let uvWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 1)
        let uvHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 1)
        let uvBytesPerRow = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1)
        guard let uvBase = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1) else { return }

        // Create textures
        let yDesc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r8Unorm, width: yWidth, height: yHeight, mipmapped: false)
        yDesc.usage = .shaderRead
        guard let yTex = device.makeTexture(descriptor: yDesc) else { return }
        yTex.replace(region: MTLRegionMake2D(0, 0, yWidth, yHeight),
                     mipmapLevel: 0, withBytes: yBase, bytesPerRow: yBytesPerRow)

        let uvDesc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rg8Unorm, width: uvWidth, height: uvHeight, mipmapped: false)
        uvDesc.usage = .shaderRead
        guard let uvTex = device.makeTexture(descriptor: uvDesc) else { return }
        uvTex.replace(region: MTLRegionMake2D(0, 0, uvWidth, uvHeight),
                      mipmapLevel: 0, withBytes: uvBase, bytesPerRow: uvBytesPerRow)

        renderFrame(yTexture: yTex, uvTexture: uvTex, drawable: drawable, rpd: rpd)
    }

    // MARK: - Metal Shaders (inline source)

    static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct VertexOut {
        float4 position [[position]];
        float2 texCoord;
    };

    // Fullscreen triangle — 3 vertices cover the entire viewport.
    // No vertex buffer needed; vertex_id generates the coordinates.
    vertex VertexOut vertexShader(uint vid [[vertex_id]]) {
        VertexOut out;
        // vid: 0 → (0,0), 1 → (2,0), 2 → (0,2)
        out.texCoord = float2((vid << 1) & 2, vid & 2);
        out.position = float4(out.texCoord * float2(2.0, -2.0)
                              + float2(-1.0, 1.0), 0.0, 1.0);
        return out;
    }

    // BT.709 video-range NV12 → RGB conversion.
    //
    // Y  in [16, 235]  → normalized [16/255, 235/255]
    // Cb in [16, 240]  → normalized [16/255, 240/255]
    // Cr in [16, 240]  → normalized [16/255, 240/255]
    //
    // BT.709 coefficients (Kr=0.2126, Kb=0.0722):
    //   R = Y' + 1.5748 * Cr'
    //   G = Y' - 0.1873 * Cb' - 0.4681 * Cr'
    //   B = Y' + 1.8556 * Cb'
    fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                   texture2d<float> yTexture  [[texture(0)]],
                                   texture2d<float> uvTexture [[texture(1)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);

        float y  = yTexture.sample(s, in.texCoord).r;
        float cb = uvTexture.sample(s, in.texCoord).r;
        float cr = uvTexture.sample(s, in.texCoord).g;

        // Video range → full range
        y  = (y  - 16.0 / 255.0) * (255.0 / 219.0);
        cb = (cb - 128.0 / 255.0) * (255.0 / 224.0);
        cr = (cr - 128.0 / 255.0) * (255.0 / 224.0);

        // BT.709
        float r = y + 1.5748 * cr;
        float g = y - 0.1873 * cb - 0.4681 * cr;
        float b = y + 1.8556 * cb;

        return float4(saturate(r), saturate(g), saturate(b), 1.0);
    }
    """
}
