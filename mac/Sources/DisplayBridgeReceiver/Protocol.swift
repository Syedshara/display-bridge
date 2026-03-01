/*
 * Protocol.swift
 * Swift mirror of shared/protocol.h.
 * All constants and struct layouts match the C definitions byte-for-byte.
 */

import Foundation

// MARK: - Network Config

let DB_DEFAULT_VIDEO_PORT: UInt16   = 5000
let DB_DEFAULT_INPUT_PORT: UInt16   = 5001
let DB_DEFAULT_CONTROL_PORT: UInt16 = 5002
let DB_SRT_LATENCY_MS: Int32       = 200

// MARK: - Video Config

let DB_TARGET_WIDTH: Int   = 2880
let DB_TARGET_HEIGHT: Int  = 1800
let DB_TARGET_FPS: Int     = 60
let DB_BITRATE_KBPS: Int   = 40000

// MARK: - Codec IDs

let DB_CODEC_HEVC: UInt8 = 1
let DB_CODEC_AV1: UInt8  = 2

// MARK: - Packet Types

let DB_PKT_VIDEO_FRAME: UInt8 = 0x01
let DB_PKT_INPUT_EVENT: UInt8 = 0x02
let DB_PKT_CONTROL: UInt8     = 0x03
let DB_PKT_HEARTBEAT: UInt8   = 0x04

// MARK: - Input Event Types

let DB_INPUT_MOUSE_MOVE: UInt8   = 0x20
let DB_INPUT_MOUSE_BUTTON: UInt8 = 0x21
let DB_INPUT_MOUSE_SCROLL: UInt8 = 0x22
let DB_INPUT_KEY_DOWN: UInt8     = 0x23
let DB_INPUT_KEY_UP: UInt8       = 0x24

// MARK: - Packet Header (16 bytes, matches db_packet_header_t)

struct DBPacketHeader {
    var type: UInt8          // DB_PKT_*
    var flags: UInt8         // reserved
    var seq: UInt16          // sequence number
    var timestamp: UInt32    // microseconds since start
    var payloadSize: UInt32  // payload size in bytes
    var reserved: UInt32

    static let size = 16

    init() {
        type = 0; flags = 0; seq = 0; timestamp = 0; payloadSize = 0; reserved = 0
    }

    init(type: UInt8, flags: UInt8, seq: UInt16, timestamp: UInt32,
         payloadSize: UInt32, reserved: UInt32 = 0) {
        self.type = type
        self.flags = flags
        self.seq = seq
        self.timestamp = timestamp
        self.payloadSize = payloadSize
        self.reserved = reserved
    }

    /// Deserialize from raw bytes (little-endian wire format).
    init?(data: Data) {
        guard data.count >= DBPacketHeader.size else { return nil }
        type        = data[data.startIndex + 0]
        flags       = data[data.startIndex + 1]
        seq         = data.loadLE(UInt16.self, offset: 2)
        timestamp   = data.loadLE(UInt32.self, offset: 4)
        payloadSize = data.loadLE(UInt32.self, offset: 8)
        reserved    = data.loadLE(UInt32.self, offset: 12)
    }

    /// Serialize to raw bytes (little-endian).
    func serialize() -> Data {
        var d = Data(count: DBPacketHeader.size)
        d[0] = type
        d[1] = flags
        d.storeLE(seq, offset: 2)
        d.storeLE(timestamp, offset: 4)
        d.storeLE(payloadSize, offset: 8)
        d.storeLE(reserved, offset: 12)
        return d
    }
}

// MARK: - Video Frame Header (16 bytes, matches db_video_header_t)

struct DBVideoHeader {
    var width: UInt16
    var height: UInt16
    var codec: UInt8
    var keyframe: UInt8
    var reserved: UInt16
    var pts: UInt64

    static let size = 16

    init?(data: Data) {
        guard data.count >= DBVideoHeader.size else { return nil }
        width    = data.loadLE(UInt16.self, offset: 0)
        height   = data.loadLE(UInt16.self, offset: 2)
        codec    = data[data.startIndex + 4]
        keyframe = data[data.startIndex + 5]
        reserved = data.loadLE(UInt16.self, offset: 6)
        pts      = data.loadLE(UInt64.self, offset: 8)
    }
}

// MARK: - Input Event (8 bytes, matches db_input_event_t)

struct DBInputEvent {
    var eventType: UInt8
    var reserved: UInt8
    var x: Int16
    var y: Int16
    var value: Int16

    static let size = 8

    init(eventType: UInt8, x: Int16, y: Int16, value: Int16) {
        self.eventType = eventType
        self.reserved = 0
        self.x = x
        self.y = y
        self.value = value
    }

    /// Serialize to raw bytes (little-endian).
    func serialize() -> Data {
        var d = Data(count: DBInputEvent.size)
        d[0] = eventType
        d[1] = reserved
        d.storeLE(x, offset: 2)
        d.storeLE(y, offset: 4)
        d.storeLE(value, offset: 6)
        return d
    }
}

// MARK: - Data helpers for little-endian load/store

extension Data {
    func loadLE<T: FixedWidthInteger>(_ type: T.Type, offset: Int) -> T {
        let start = self.startIndex + offset
        var value: T = 0
        _ = Swift.withUnsafeMutableBytes(of: &value) { dst in
            self.copyBytes(to: dst, from: start ..< start + MemoryLayout<T>.size)
        }
        return T(littleEndian: value)
    }

    mutating func storeLE<T: FixedWidthInteger>(_ value: T, offset: Int) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { src in
            for i in 0 ..< MemoryLayout<T>.size {
                self[self.startIndex + offset + i] = src[i]
            }
        }
    }
}
