/*
 * SRTReceiver.swift
 * SRT caller (client) — connects to the Ubuntu sender and receives
 * HEVC video frames in message mode.
 *
 * Architecture:
 *   - Runs a recv loop on a dedicated background DispatchQueue.
 *   - Connects to Ubuntu as SRT caller (Ubuntu is listener/server).
 *   - Transport: SRTT_FILE + SRTO_MESSAGEAPI=1 (matches streamer.c exactly).
 *   - Each srt_recvmsg call returns exactly one complete frame:
 *     db_packet_header_t (16) + db_video_header_t (16) + HEVC bitstream.
 *   - On connection failure, retries every 1 second.
 */

import Foundation
import CSRT

// MARK: - Delegate

protocol SRTReceiverDelegate: AnyObject {
    func srtReceiver(_ receiver: SRTReceiver,
                     didReceiveVideoFrame header: DBVideoHeader,
                     data: Data)
    func srtReceiverDidConnect(_ receiver: SRTReceiver)
    func srtReceiverDidDisconnect(_ receiver: SRTReceiver)
}

// MARK: - SRTReceiver

final class SRTReceiver {

    // Default IP — overridden by ServiceDiscovery when available.
    // TODO: Set this to the Ubuntu machine's WiFi IP if mDNS fails.
    static let DEFAULT_UBUNTU_IP = "10.135.95.11"

    weak var delegate: SRTReceiverDelegate?

    /// The target IP to connect to. Can be updated before start() or
    /// after stop() to point to a discovered host.
    var targetHost: String = SRTReceiver.DEFAULT_UBUNTU_IP

    private let queue = DispatchQueue(label: "com.display-bridge.srt-recv",
                                      qos: .userInteractive)
    private let sockLock = NSLock()
    private var _sock: SRTSOCKET = SRT_INVALID_SOCK
    private var sock: SRTSOCKET {
        get { sockLock.lock(); defer { sockLock.unlock() }; return _sock }
        set { sockLock.lock(); defer { sockLock.unlock() }; _sock = newValue }
    }
    private var running = false
    private let maxBufSize = 8 * 1024 * 1024  // 8 MB — well above any single frame

    // MARK: - Lifecycle

    init() {
        srt_startup()
        let ver = srt_getversion()
        let major = (ver >> 16) & 0xff
        let minor = (ver >> 8) & 0xff
        let patch = ver & 0xff
        log("SRT library started (version \(major).\(minor).\(patch))")
    }

    deinit {
        stop()
        srt_cleanup()
    }

    /// Start the recv loop on a background thread.
    func start() {
        guard !running else { return }
        running = true
        queue.async { [weak self] in
            self?.recvLoop()
        }
    }

    /// Signal the recv loop to stop and close the socket.
    func stop() {
        running = false
        let s = sock
        if s != SRT_INVALID_SOCK {
            srt_close(s)
            sock = SRT_INVALID_SOCK
        }
    }

    // MARK: - Connection

    private func connect() -> Bool {
        // Create socket
        let s = srt_create_socket()
        guard s != SRT_INVALID_SOCK else {
            log("ERROR: srt_create_socket failed: \(srtError())")
            return false
        }

        // Set socket options (must match streamer.c)
        var transtype = SRTT_FILE
        srt_setsockflag(s, SRTO_TRANSTYPE, &transtype,
                         Int32(MemoryLayout.size(ofValue: transtype)))

        var msgapi: Int32 = 1
        srt_setsockflag(s, SRTO_MESSAGEAPI, &msgapi,
                         Int32(MemoryLayout<Int32>.size))

        var latency: Int32 = DB_SRT_LATENCY_MS
        srt_setsockflag(s, SRTO_LATENCY, &latency,
                         Int32(MemoryLayout<Int32>.size))

        var rcvbuf: Int32 = Int32(maxBufSize)
        srt_setsockflag(s, SRTO_RCVBUF, &rcvbuf,
                         Int32(MemoryLayout<Int32>.size))

        // Strip zone ID suffix (e.g. "2401:...%en0" → "2401:...")
        let rawIP = targetHost
        let cleanIP: String
        if let pct = rawIP.firstIndex(of: "%") {
            cleanIP = String(rawIP[rawIP.startIndex..<pct])
        } else {
            cleanIP = rawIP
        }

        let result: Int32
        if cleanIP.contains(":") {
            // ── IPv6 path ──────────────────────────────────────────────────
            var addr = sockaddr_in6()
            addr.sin6_family = sa_family_t(AF_INET6)
            addr.sin6_port = DB_DEFAULT_VIDEO_PORT.bigEndian
            guard inet_pton(AF_INET6, cleanIP, &addr.sin6_addr) == 1 else {
                log("ERROR: invalid IPv6 address '\(cleanIP)'")
                srt_close(s)
                return false
            }
            result = withUnsafePointer(to: &addr) { addrPtr in
                addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    srt_connect(s, sa, Int32(MemoryLayout<sockaddr_in6>.size))
                }
            }
            guard result != SRT_ERROR else {
                log("connect to [\(cleanIP)]:\(DB_DEFAULT_VIDEO_PORT) failed: \(srtError())")
                srt_close(s)
                return false
            }
            sock = s
            log("connected to [\(cleanIP)]:\(DB_DEFAULT_VIDEO_PORT)")
        } else {
            // ── IPv4 path ──────────────────────────────────────────────────
            var addr = sockaddr_in()
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = DB_DEFAULT_VIDEO_PORT.bigEndian
            guard inet_pton(AF_INET, cleanIP, &addr.sin_addr) == 1 else {
                log("ERROR: invalid IP address '\(cleanIP)'")
                srt_close(s)
                return false
            }
            result = withUnsafePointer(to: &addr) { addrPtr in
                addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    srt_connect(s, sa, Int32(MemoryLayout<sockaddr_in>.size))
                }
            }
            guard result != SRT_ERROR else {
                log("connect to \(cleanIP):\(DB_DEFAULT_VIDEO_PORT) failed: \(srtError())")
                srt_close(s)
                return false
            }
            sock = s
            log("connected to \(cleanIP):\(DB_DEFAULT_VIDEO_PORT)")
        }

        return true
    }

    // MARK: - Recv Loop

    private func recvLoop() {
        let buf = UnsafeMutablePointer<CChar>.allocate(capacity: maxBufSize)
        defer { buf.deallocate() }

        while running {
            // Connect (retry loop)
            while running && sock == SRT_INVALID_SOCK {
                if connect() {
                    DispatchQueue.main.async { [weak self] in
                        guard let self = self else { return }
                        self.delegate?.srtReceiverDidConnect(self)
                    }
                    break
                }
                // Wait before retry
                Thread.sleep(forTimeInterval: 1.0)
            }
            guard running else { break }

            // Receive one message (= one frame)
            let recvd = srt_recvmsg(sock, buf, Int32(maxBufSize))
            if recvd == SRT_ERROR {
                let errCode = srt_getlasterror(nil)
                // Connection broken or closed
                if errCode == SRT_ECONNLOST.rawValue ||
                   errCode == SRT_ENOCONN.rawValue ||
                   !running {
                    log("connection lost (err=\(errCode)), reconnecting...")
                    srt_close(sock)
                    sock = SRT_INVALID_SOCK
                    DispatchQueue.main.async { [weak self] in
                        guard let self = self else { return }
                        self.delegate?.srtReceiverDidDisconnect(self)
                    }
                    continue
                }
                // Transient error — retry recv
                log("WARNING: srt_recvmsg error: \(srtError())")
                continue
            }

            let totalSize = Int(recvd)
            let minSize = DBPacketHeader.size + DBVideoHeader.size
            guard totalSize >= minSize else {
                log("WARNING: undersized message (\(totalSize) bytes)")
                continue
            }

            // Parse packet header
            let rawData = Data(bytes: buf, count: totalSize)
            guard let pktHeader = DBPacketHeader(data: rawData) else {
                log("WARNING: failed to parse packet header")
                continue
            }

            switch pktHeader.type {
            case DB_PKT_VIDEO_FRAME:
                let videoData = rawData.dropFirst(DBPacketHeader.size)
                guard let vidHeader = DBVideoHeader(data: videoData) else {
                    log("WARNING: failed to parse video header")
                    continue
                }
                let hevcData = rawData.dropFirst(DBPacketHeader.size + DBVideoHeader.size)
                delegate?.srtReceiver(self, didReceiveVideoFrame: vidHeader, data: Data(hevcData))

            case DB_PKT_HEARTBEAT:
                break  // ignore heartbeats

            default:
                log("WARNING: unknown packet type 0x\(String(pktHeader.type, radix: 16))")
            }
        }

        // Cleanup
        if sock != SRT_INVALID_SOCK {
            srt_close(sock)
            sock = SRT_INVALID_SOCK
        }
        log("recv loop exited")
    }

    // MARK: - Helpers

    private func srtError() -> String {
        return String(cString: srt_getlasterror_str())
    }

    private func log(_ msg: String) {
        let ts = String(format: "%.3f", CFAbsoluteTimeGetCurrent())
        print("[\(ts)] [srt-recv] \(msg)")
    }
}
