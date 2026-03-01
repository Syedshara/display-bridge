/*
 * ViewController.swift
 * Pipeline glue — owns all five components and wires their delegates.
 *
 * Startup sequence:
 *   1. Create MetalRenderer, HEVCDecoder, SRTReceiver, InputForwarder
 *   2. Start ServiceDiscovery to find Ubuntu sender via mDNS
 *   3. On discovery success: set targetHost on SRTReceiver + InputForwarder
 *      On discovery timeout: fall back to DEFAULT_UBUNTU_IP
 *   4. Start SRTReceiver (begins connect loop on background thread)
 *
 * Data flow:
 *   SRTReceiver [bg thread]
 *     -> srtReceiver(_:didReceiveVideoFrame:data:)
 *     -> HEVCDecoder.decode(frameData:header:)  [bg thread]
 *     -> VTDecompressionSession callback [VT thread]
 *     -> hevcDecoder(_:didOutput:pts:)  [dispatched to main thread]
 *     -> MetalRenderer.display(pixelBuffer:)  [main thread]
 *
 * Input flow:
 *   NSEvent [main thread]
 *     -> InputForwarder.handleEvent(_:)
 *     -> UDP sendto(targetHost:5001)
 */

import AppKit
import CoreMedia
import MetalKit

final class ViewController: NSViewController {

    private(set) var metalRenderer: MetalRenderer?
    private var srtReceiver: SRTReceiver?
    private var hevcDecoder: HEVCDecoder?
    private var inputForwarder: InputForwarder?
    private var serviceDiscovery: ServiceDiscovery?

    // Stats
    private var framesReceived: UInt64 = 0
    private var framesDecoded: UInt64 = 0
    private var statsTimer: Timer?

    // MARK: - View Lifecycle

    override func loadView() {
        guard let renderer = MetalRenderer.make() else {
            fatalError("[vc] failed to initialize MetalRenderer -- no Metal device?")
        }
        metalRenderer = renderer
        self.view = renderer.view
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        // Create decoder
        let decoder = HEVCDecoder()
        decoder.delegate = self
        hevcDecoder = decoder

        // Create SRT receiver (not started yet — waiting for discovery)
        let receiver = SRTReceiver()
        receiver.delegate = self
        srtReceiver = receiver

        // Create input forwarder
        if let rendererView = metalRenderer?.view {
            inputForwarder = InputForwarder(view: rendererView)
        }

        // Start mDNS discovery to find the Ubuntu sender
        let discovery = ServiceDiscovery()
        discovery.delegate = self
        serviceDiscovery = discovery
        discovery.startBrowsing()
        print("[vc] searching for Ubuntu sender via mDNS...")
    }

    override func viewDidAppear() {
        super.viewDidAppear()

        // Start input forwarding once the window is visible
        inputForwarder?.startMonitoring()

        // Make the Metal view first responder for keyboard events
        view.window?.makeFirstResponder(metalRenderer?.view)

        // Start stats logging (every 5 seconds)
        statsTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            self?.logStats()
        }
    }

    override func viewWillDisappear() {
        super.viewWillDisappear()
        statsTimer?.invalidate()
        statsTimer = nil
    }

    // MARK: - Pipeline Start

    /// Configure target host and start the SRT receiver + input pipeline.
    private func startPipeline(host: String) {
        // Belt-and-suspenders: strip IPv6 zone ID suffix (e.g. "%en0") if present
        let cleanHost: String
        if let pct = host.firstIndex(of: "%") {
            cleanHost = String(host[host.startIndex..<pct])
        } else {
            cleanHost = host
        }

        print("[vc] target host: \(cleanHost)")

        srtReceiver?.targetHost = cleanHost
        inputForwarder?.targetHost = cleanHost

        srtReceiver?.start()
        print("[vc] pipeline started, waiting for SRT connection...")
    }

    // MARK: - Shutdown

    func shutdown() {
        statsTimer?.invalidate()
        serviceDiscovery?.stopBrowsing()
        inputForwarder?.stopMonitoring()
        srtReceiver?.stop()
        hevcDecoder?.invalidate()
        print("[vc] pipeline shutdown")
    }

    // MARK: - Stats

    private func logStats() {
        print("[vc] stats: received=\(framesReceived), decoded=\(framesDecoded)")
    }
}

// MARK: - ServiceDiscoveryDelegate

extension ViewController: ServiceDiscoveryDelegate {
    func serviceDiscovery(_ discovery: ServiceDiscovery,
                          didFindSender host: String, port: UInt16) {
        print("[vc] mDNS discovered sender at \(host):\(port)")
        startPipeline(host: host)
    }

    func serviceDiscoveryDidFail(_ discovery: ServiceDiscovery) {
        let fallback = SRTReceiver.DEFAULT_UBUNTU_IP
        print("[vc] mDNS discovery failed, falling back to \(fallback)")
        startPipeline(host: fallback)
    }
}

// MARK: - SRTReceiverDelegate

extension ViewController: SRTReceiverDelegate {
    func srtReceiver(_ receiver: SRTReceiver,
                     didReceiveVideoFrame header: DBVideoHeader,
                     data: Data) {
        framesReceived += 1
        // Decode on the SRT recv thread — VT handles internal threading
        hevcDecoder?.decode(frameData: data, header: header)
    }

    func srtReceiverDidConnect(_ receiver: SRTReceiver) {
        print("[vc] SRT connected -- receiving video")
    }

    func srtReceiverDidDisconnect(_ receiver: SRTReceiver) {
        print("[vc] SRT disconnected -- waiting for reconnect...")
    }
}

// MARK: - HEVCDecoderDelegate

extension ViewController: HEVCDecoderDelegate {
    func hevcDecoder(_ decoder: HEVCDecoder,
                     didOutput pixelBuffer: CVPixelBuffer,
                     pts: CMTime) {
        // Already on main thread (dispatched by HEVCDecoder)
        framesDecoded += 1
        metalRenderer?.display(pixelBuffer: pixelBuffer)
    }
}
