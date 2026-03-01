/*
 * HEVCDecoder.swift
 * VideoToolbox HEVC decoder.
 *
 * Receives raw Annex B HEVC bitstream from the SRT receiver and produces
 * CVPixelBuffer (NV12) frames via VTDecompressionSession.
 *
 * Pipeline:
 *   1. Parse Annex B NALUs (split on 00 00 00 01 start codes).
 *   2. On IDR frame: extract VPS/SPS/PPS → CMVideoFormatDescription.
 *   3. Convert slice NALUs from Annex B → AVCC (4-byte length prefix).
 *   4. Wrap in CMBlockBuffer → CMSampleBuffer → VTDecompressionSessionDecodeFrame.
 *   5. Output CVPixelBuffer via delegate callback.
 *
 * HEVC NALU types from our encoder:
 *   0  = TRAIL_N       (unused by our encoder)
 *   1  = TRAIL_R       (P-frame slice)
 *   19 = IDR_W_RADL    (IDR slice)
 *   32 = VPS_NUT
 *   33 = SPS_NUT
 *   34 = PPS_NUT
 *
 * NALU type extraction: naluType = (byte0 >> 1) & 0x3F
 */

import Foundation
import CoreMedia
import CoreVideo
import VideoToolbox

// MARK: - Delegate

protocol HEVCDecoderDelegate: AnyObject {
    func hevcDecoder(_ decoder: HEVCDecoder,
                     didOutput pixelBuffer: CVPixelBuffer,
                     pts: CMTime)
}

// MARK: - NALU

private struct NALU {
    let type: UInt8     // HEVC NALU type (0-63)
    let data: Data      // Raw NALU bytes WITHOUT start code (includes 2-byte NALU header)
}

// MARK: - HEVCDecoder

final class HEVCDecoder {

    weak var delegate: HEVCDecoderDelegate?

    private var session: VTDecompressionSession?
    private var formatDescription: CMFormatDescription?

    // Cache latest parameter sets to detect changes
    private var lastVPS: Data?
    private var lastSPS: Data?
    private var lastPPS: Data?

    private var frameCount: UInt64 = 0
    private var waitingForIDR = true

    // MARK: - Public API

    /// Decode a single frame's HEVC bitstream (Annex B format).
    /// Called from the SRT recv thread. VT decode callback dispatches to main.
    func decode(frameData: Data, header: DBVideoHeader) {
        let nalus = parseAnnexB(frameData)
        if nalus.isEmpty {
            log("WARNING: no NALUs found in frame (\(frameData.count) bytes)")
            return
        }

        // Extract parameter sets on IDR frames
        if header.keyframe == 1 {
            let vps = nalus.first(where: { $0.type == 32 })?.data
            let sps = nalus.first(where: { $0.type == 33 })?.data
            let pps = nalus.first(where: { $0.type == 34 })?.data

            if let vps = vps, let sps = sps, let pps = pps {
                if vps != lastVPS || sps != lastSPS || pps != lastPPS {
                    if updateFormatDescription(vps: vps, sps: sps, pps: pps) {
                        lastVPS = vps
                        lastSPS = sps
                        lastPPS = pps
                        // Recreate session with new format
                        createDecompressionSession()
                    }
                }
                waitingForIDR = false
            } else {
                log("WARNING: IDR frame missing parameter sets "
                    + "(VPS=\(vps != nil) SPS=\(sps != nil) PPS=\(pps != nil))")
                return
            }
        }

        // Drop frames until we have a valid session
        if waitingForIDR {
            return
        }
        guard let session = session, let fmtDesc = formatDescription else {
            return
        }

        // Collect all slice NALUs (type 0-31 in HEVC are VCL NALUs)
        let sliceNALUs = nalus.filter { $0.type <= 31 }
        if sliceNALUs.isEmpty {
            return
        }

        // Convert to AVCC format (4-byte BE length prefix instead of start code)
        var avccData = Data()
        for nalu in sliceNALUs {
            var length = CFSwapInt32HostToBig(UInt32(nalu.data.count))
            avccData.append(Data(bytes: &length, count: 4))
            avccData.append(nalu.data)
        }

        // Create CMBlockBuffer
        guard let blockBuffer = createBlockBuffer(from: avccData) else {
            log("ERROR: failed to create CMBlockBuffer")
            return
        }

        // Create CMSampleBuffer
        var sampleBuffer: CMSampleBuffer?
        var timingInfo = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: Int32(DB_TARGET_FPS)),
            presentationTimeStamp: CMTime(value: CMTimeValue(header.pts),
                                          timescale: 1_000_000),
            decodeTimeStamp: .invalid
        )
        var sampleSize = avccData.count

        let sbStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: fmtDesc,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timingInfo,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard sbStatus == noErr, let sb = sampleBuffer else {
            log("ERROR: CMSampleBufferCreateReady failed (\(sbStatus))")
            return
        }

        // Decode
        let decodeFlags: VTDecodeFrameFlags = [._EnableAsynchronousDecompression,
                                                ._1xRealTimePlayback]
        var infoFlags = VTDecodeInfoFlags()

        let vtStatus = VTDecompressionSessionDecodeFrame(
            session,
            sampleBuffer: sb,
            flags: decodeFlags,
            infoFlagsOut: &infoFlags
        ) { [weak self] status, _, imageBuffer, presentationTimeStamp, _ in
            guard let self = self else { return }
            guard status == noErr else {
                self.log("WARNING: VT decode callback error \(status)")
                return
            }
            guard let pixelBuffer = imageBuffer else {
                self.log("WARNING: VT decode returned nil imageBuffer")
                return
            }
            let cvpb = pixelBuffer as CVPixelBuffer
            let pts = presentationTimeStamp
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.frameCount += 1
                self.delegate?.hevcDecoder(self, didOutput: cvpb, pts: pts)
            }
        }

        if vtStatus != noErr {
            log("ERROR: VTDecompressionSessionDecodeFrame failed (\(vtStatus))")
        }
    }

    /// Flush pending frames and tear down session.
    func invalidate() {
        if let session = session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        formatDescription = nil
        waitingForIDR = true
    }

    // MARK: - Format Description

    private func updateFormatDescription(vps: Data, sps: Data, pps: Data) -> Bool {
        var fmtDesc: CMFormatDescription?

        let status = vps.withUnsafeBytes { vpsRaw -> OSStatus in
            sps.withUnsafeBytes { spsRaw -> OSStatus in
                pps.withUnsafeBytes { ppsRaw -> OSStatus in
                    let pointers: [UnsafePointer<UInt8>] = [
                        vpsRaw.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        spsRaw.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        ppsRaw.baseAddress!.assumingMemoryBound(to: UInt8.self)
                    ]
                    let sizes = [vps.count, sps.count, pps.count]
                    return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: 3,
                        parameterSetPointers: pointers,
                        parameterSetSizes: sizes,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &fmtDesc
                    )
                }
            }
        }

        guard status == noErr, let desc = fmtDesc else {
            log("ERROR: CMVideoFormatDescriptionCreateFromHEVCParameterSets failed (\(status))")
            return false
        }

        let dims = CMVideoFormatDescriptionGetDimensions(desc)
        log("format description created: \(dims.width)x\(dims.height) HEVC")
        formatDescription = desc
        return true
    }

    // MARK: - Decompression Session

    private func createDecompressionSession() {
        // Tear down existing session
        if let old = session {
            VTDecompressionSessionWaitForAsynchronousFrames(old)
            VTDecompressionSessionInvalidate(old)
            session = nil
        }

        guard let fmtDesc = formatDescription else { return }

        // Request NV12 output, Metal-compatible, IOSurface-backed
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String:
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any]
        ]

        var newSession: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: fmtDesc,
            decoderSpecification: nil,
            imageBufferAttributes: attrs as CFDictionary,
            outputCallback: nil,   // using per-frame block callback
            decompressionSessionOut: &newSession
        )

        guard status == noErr, let s = newSession else {
            log("ERROR: VTDecompressionSessionCreate failed (\(status))")
            return
        }

        session = s
        log("decompression session created")
    }

    // MARK: - CMBlockBuffer

    private func createBlockBuffer(from data: Data) -> CMBlockBuffer? {
        let length = data.count
        var blockBuffer: CMBlockBuffer?

        // Allocate block buffer with internal memory
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: length,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: length,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard status == kCMBlockBufferNoErr, let bb = blockBuffer else {
            return nil
        }

        // Copy our data into the block buffer
        status = data.withUnsafeBytes { ptr in
            CMBlockBufferReplaceDataBytes(
                with: ptr.baseAddress!,
                blockBuffer: bb,
                offsetIntoDestination: 0,
                dataLength: length
            )
        }
        guard status == kCMBlockBufferNoErr else {
            return nil
        }

        return bb
    }

    // MARK: - Annex B Parser

    /// Parse Annex B bitstream into individual NALUs.
    /// Handles both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
    private func parseAnnexB(_ data: Data) -> [NALU] {
        var nalus: [NALU] = []
        let bytes = [UInt8](data)
        let count = bytes.count
        guard count > 4 else { return nalus }

        // Find start code positions
        var startPositions: [(offset: Int, naluStart: Int)] = []
        var i = 0
        while i < count - 2 {
            if bytes[i] == 0 && bytes[i + 1] == 0 {
                if i + 3 < count && bytes[i + 2] == 0 && bytes[i + 3] == 1 {
                    // 4-byte start code
                    startPositions.append((offset: i, naluStart: i + 4))
                    i += 4
                    continue
                } else if bytes[i + 2] == 1 {
                    // 3-byte start code
                    startPositions.append((offset: i, naluStart: i + 3))
                    i += 3
                    continue
                }
            }
            i += 1
        }

        // Extract NALUs between start codes
        for j in 0 ..< startPositions.count {
            let naluStart = startPositions[j].naluStart
            let naluEnd: Int
            if j + 1 < startPositions.count {
                naluEnd = startPositions[j + 1].offset
            } else {
                naluEnd = count
            }

            guard naluEnd > naluStart else { continue }

            let naluData = Data(bytes[naluStart ..< naluEnd])
            // HEVC NALU type: (first_byte >> 1) & 0x3F
            let naluType = (naluData[0] >> 1) & 0x3F
            nalus.append(NALU(type: naluType, data: naluData))
        }

        return nalus
    }

    // MARK: - Logging

    private func log(_ msg: String) {
        print("[decoder] \(msg)")
    }
}
