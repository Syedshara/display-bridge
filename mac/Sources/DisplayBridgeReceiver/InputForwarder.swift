/*
 * InputForwarder.swift
 * Captures keyboard and mouse events from the receiver window and sends
 * them as UDP datagrams to the Ubuntu sender on port 5001.
 *
 * Architecture:
 *   - Uses NSEvent.addLocalMonitorForEvents to capture events within our
 *     full-screen window (no Accessibility permission needed).
 *   - Converts macOS keyCode → Linux KEY_* via lookup table.
 *   - Mouse coordinates: normalized to 0..DB_TARGET_WIDTH/HEIGHT range
 *     (absolute coordinates, matching Ubuntu's uinput device).
 *   - Sends: db_packet_header_t (16 bytes) + db_input_event_t (8 bytes)
 *     per event, via raw UDP to Ubuntu_IP:5001.
 */

import Foundation
import AppKit

// MARK: - InputForwarder

final class InputForwarder {

    private weak var view: NSView?
    private var udpSocket: Int32 = -1
    private var destAddr = sockaddr_in()
    private var monitor: Any?
    private var seq: UInt16 = 0
    private let startTime = CFAbsoluteTimeGetCurrent()

    /// The target host for input events. Must be set before startMonitoring().
    var targetHost: String = SRTReceiver.DEFAULT_UBUNTU_IP {
        didSet {
            if udpSocket >= 0 {
                updateDestAddr()
            }
        }
    }

    // MARK: - Init

    init(view: NSView) {
        self.view = view
        setupUDP()
    }

    deinit {
        stopMonitoring()
        if udpSocket >= 0 {
            close(udpSocket)
        }
    }

    // MARK: - UDP Setup

    private func setupUDP() {
        udpSocket = socket(AF_INET, SOCK_DGRAM, 0)
        guard udpSocket >= 0 else {
            log("ERROR: failed to create UDP socket: \(String(cString: strerror(errno)))")
            return
        }

        destAddr.sin_family = sa_family_t(AF_INET)
        destAddr.sin_port = DB_DEFAULT_INPUT_PORT.bigEndian
        inet_pton(AF_INET, targetHost, &destAddr.sin_addr)

        log("UDP socket ready -> \(targetHost):\(DB_DEFAULT_INPUT_PORT)")
    }

    /// Update the destination address when targetHost changes at runtime.
    private func updateDestAddr() {
        destAddr.sin_family = sa_family_t(AF_INET)
        destAddr.sin_port = DB_DEFAULT_INPUT_PORT.bigEndian
        inet_pton(AF_INET, targetHost, &destAddr.sin_addr)
        log("destination updated -> \(targetHost):\(DB_DEFAULT_INPUT_PORT)")
    }

    // MARK: - Event Monitoring

    func startMonitoring() {
        guard monitor == nil else { return }

        let mask: NSEvent.EventTypeMask = [
            .mouseMoved, .leftMouseDragged, .rightMouseDragged,
            .leftMouseDown, .leftMouseUp,
            .rightMouseDown, .rightMouseUp,
            .keyDown, .keyUp,
            .scrollWheel,
            .flagsChanged
        ]

        monitor = NSEvent.addLocalMonitorForEvents(matching: mask) { [weak self] event in
            self?.handleEvent(event)

            // Consume keyboard events (don't trigger Mac shortcuts)
            switch event.type {
            case .keyDown, .keyUp:
                return nil
            default:
                return event
            }
        }

        log("event monitoring started")
    }

    func stopMonitoring() {
        if let m = monitor {
            NSEvent.removeMonitor(m)
            monitor = nil
            log("event monitoring stopped")
        }
    }

    // MARK: - Event Handling

    private func handleEvent(_ event: NSEvent) {
        guard let view = view else { return }

        switch event.type {
        case .mouseMoved, .leftMouseDragged, .rightMouseDragged:
            let (x, y) = mapCoordinates(event: event, view: view)
            send(eventType: DB_INPUT_MOUSE_MOVE, x: x, y: y, value: 0)

        case .leftMouseDown:
            let (x, y) = mapCoordinates(event: event, view: view)
            send(eventType: DB_INPUT_MOUSE_BUTTON, x: x, y: y, value: 1)  // BTN_LEFT

        case .leftMouseUp:
            let (x, y) = mapCoordinates(event: event, view: view)
            send(eventType: DB_INPUT_MOUSE_BUTTON, x: x, y: y, value: -1) // BTN_LEFT release

        case .rightMouseDown:
            let (x, y) = mapCoordinates(event: event, view: view)
            send(eventType: DB_INPUT_MOUSE_BUTTON, x: x, y: y, value: 2)  // BTN_RIGHT

        case .rightMouseUp:
            let (x, y) = mapCoordinates(event: event, view: view)
            send(eventType: DB_INPUT_MOUSE_BUTTON, x: x, y: y, value: -2) // BTN_RIGHT release

        case .scrollWheel:
            let (x, y) = mapCoordinates(event: event, view: view)
            let delta = Int16(clamping: Int(event.scrollingDeltaY * 10))
            send(eventType: DB_INPUT_MOUSE_SCROLL, x: x, y: y, value: delta)

        case .keyDown:
            let linuxCode = macToLinuxKey(event.keyCode)
            send(eventType: DB_INPUT_KEY_DOWN, x: Int16(linuxCode), y: 0, value: 0)

        case .keyUp:
            let linuxCode = macToLinuxKey(event.keyCode)
            send(eventType: DB_INPUT_KEY_UP, x: Int16(linuxCode), y: 0, value: 0)

        case .flagsChanged:
            // Handle modifier key press/release via flagsChanged
            handleModifierFlags(event)

        default:
            break
        }
    }

    // MARK: - Modifier Flags

    private var lastModifierFlags: NSEvent.ModifierFlags = []

    private func handleModifierFlags(_ event: NSEvent) {
        let current = event.modifierFlags
        let changed = current.symmetricDifference(lastModifierFlags)
        lastModifierFlags = current

        // Check each modifier key
        let modifiers: [(NSEvent.ModifierFlags, UInt16, UInt16)] = [
            (.shift,   0x38, 42),   // Left Shift → KEY_LEFTSHIFT
            (.control, 0x3B, 29),   // Left Control → KEY_LEFTCTRL
            (.option,  0x3A, 56),   // Left Option → KEY_LEFTALT
            (.command, 0x37, 125),  // Command → KEY_LEFTMETA
        ]

        for (flag, _, linuxCode) in modifiers {
            if changed.contains(flag) {
                let isDown = current.contains(flag)
                let eventType = isDown ? DB_INPUT_KEY_DOWN : DB_INPUT_KEY_UP
                send(eventType: eventType, x: Int16(linuxCode), y: 0, value: 0)
            }
        }
    }

    // MARK: - Coordinate Mapping

    /// Convert NSEvent window coordinates → target display coordinates.
    /// macOS: origin bottom-left, Y up. Linux: origin top-left, Y down.
    private func mapCoordinates(event: NSEvent, view: NSView) -> (Int16, Int16) {
        let bounds = view.bounds
        guard bounds.width > 0 && bounds.height > 0 else { return (0, 0) }

        let loc = view.convert(event.locationInWindow, from: nil)
        let normX = max(0, min(1, loc.x / bounds.width))
        let normY = max(0, min(1, 1.0 - loc.y / bounds.height))  // flip Y

        let targetX = Int16(normX * Double(DB_TARGET_WIDTH - 1))
        let targetY = Int16(normY * Double(DB_TARGET_HEIGHT - 1))
        return (targetX, targetY)
    }

    // MARK: - Send

    private func send(eventType: UInt8, x: Int16, y: Int16, value: Int16) {
        guard udpSocket >= 0 else { return }

        let now = UInt32((CFAbsoluteTimeGetCurrent() - startTime) * 1_000_000)
        let inputEvt = DBInputEvent(eventType: eventType, x: x, y: y, value: value)
        let inputData = inputEvt.serialize()

        var pkt = DBPacketHeader(
            type: DB_PKT_INPUT_EVENT,
            flags: 0,
            seq: seq,
            timestamp: now,
            payloadSize: UInt32(inputData.count)
        )
        seq &+= 1

        var packet = pkt.serialize()
        packet.append(inputData)

        packet.withUnsafeBytes { rawBuf in
            withUnsafePointer(to: &destAddr) { addrPtr in
                addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    _ = sendto(udpSocket, rawBuf.baseAddress, rawBuf.count,
                               0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    // MARK: - macOS keyCode → Linux KEY_* mapping

    private func macToLinuxKey(_ macCode: UInt16) -> UInt16 {
        return InputForwarder.keyMap[macCode] ?? 0
    }

    /// macOS hardware keyCode → Linux input-event-codes.h KEY_* value.
    /// Covers all alphanumeric, punctuation, modifiers, arrows, and F1-F12.
    private static let keyMap: [UInt16: UInt16] = [
        // Letters (macOS keyCode → Linux KEY_*)
        0x00: 30,   // A
        0x01: 31,   // S
        0x02: 32,   // D
        0x03: 33,   // F
        0x04: 35,   // H
        0x05: 34,   // G
        0x06: 44,   // Z
        0x07: 45,   // X
        0x08: 46,   // C
        0x09: 47,   // V
        0x0B: 48,   // B
        0x0C: 16,   // Q
        0x0D: 17,   // W
        0x0E: 18,   // E
        0x0F: 19,   // R
        0x10: 21,   // Y
        0x11: 20,   // T
        0x1F: 24,   // O
        0x20: 22,   // U
        0x22: 23,   // I
        0x23: 25,   // P
        0x25: 38,   // L
        0x26: 36,   // J
        0x28: 37,   // K
        0x2D: 49,   // N
        0x2E: 50,   // M

        // Numbers
        0x12: 2,    // 1
        0x13: 3,    // 2
        0x14: 4,    // 3
        0x15: 5,    // 4
        0x17: 6,    // 5
        0x16: 7,    // 6
        0x1A: 8,    // 7
        0x1C: 9,    // 8
        0x19: 10,   // 9
        0x1D: 11,   // 0

        // Punctuation / symbols
        0x18: 13,   // Equal
        0x1B: 12,   // Minus
        0x1E: 27,   // RightBracket
        0x21: 26,   // LeftBracket
        0x27: 40,   // Quote / Apostrophe
        0x29: 39,   // Semicolon
        0x2A: 43,   // Backslash
        0x2B: 51,   // Comma
        0x2C: 53,   // Slash
        0x2F: 52,   // Period / Dot
        0x32: 41,   // Grave / Backtick

        // Control keys
        0x24: 28,   // Return → KEY_ENTER
        0x30: 15,   // Tab → KEY_TAB
        0x31: 57,   // Space → KEY_SPACE
        0x33: 14,   // Delete (Backspace) → KEY_BACKSPACE
        0x35: 1,    // Escape → KEY_ESC
        0x39: 58,   // CapsLock → KEY_CAPSLOCK

        // Modifier keys
        0x37: 125,  // Command → KEY_LEFTMETA
        0x38: 42,   // Left Shift → KEY_LEFTSHIFT
        0x3A: 56,   // Left Option → KEY_LEFTALT
        0x3B: 29,   // Left Control → KEY_LEFTCTRL
        0x3C: 54,   // Right Shift → KEY_RIGHTSHIFT
        0x3D: 100,  // Right Option → KEY_RIGHTALT
        0x3E: 97,   // Right Control → KEY_RIGHTCTRL

        // Arrow keys
        0x7B: 105,  // Left Arrow → KEY_LEFT
        0x7C: 106,  // Right Arrow → KEY_RIGHT
        0x7D: 108,  // Down Arrow → KEY_DOWN
        0x7E: 103,  // Up Arrow → KEY_UP

        // Function keys
        0x7A: 59,   // F1
        0x78: 60,   // F2
        0x63: 61,   // F3
        0x76: 62,   // F4
        0x60: 63,   // F5
        0x61: 64,   // F6
        0x62: 65,   // F7
        0x64: 66,   // F8
        0x65: 67,   // F9
        0x6D: 68,   // F10
        0x67: 87,   // F11
        0x6F: 88,   // F12

        // Additional keys
        0x75: 111,  // Forward Delete → KEY_DELETE
        0x73: 102,  // Home → KEY_HOME
        0x77: 107,  // End → KEY_END
        0x74: 104,  // Page Up → KEY_PAGEUP
        0x79: 109,  // Page Down → KEY_PAGEDOWN
    ]

    // MARK: - Logging

    private func log(_ msg: String) {
        print("[input] \(msg)")
    }
}
