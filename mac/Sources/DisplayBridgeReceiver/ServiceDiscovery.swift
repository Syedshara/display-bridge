/*
 * ServiceDiscovery.swift
 * Bonjour/mDNS service discovery for finding the Ubuntu sender.
 *
 * Uses NWBrowser (Network framework) to browse for "_display-bridge._tcp"
 * services on the local network. When a service is found, resolves the
 * endpoint to an IP address and notifies the delegate.
 *
 * Falls back to the hardcoded IP in SRTReceiver if no service is found
 * within the timeout period.
 *
 * Requires macOS 13+ (Network framework NWBrowser API).
 */

import Foundation
import Network
import Darwin

// MARK: - Delegate

protocol ServiceDiscoveryDelegate: AnyObject {
    func serviceDiscovery(_ discovery: ServiceDiscovery,
                          didFindSender host: String, port: UInt16)
    func serviceDiscoveryDidFail(_ discovery: ServiceDiscovery)
}

// MARK: - ServiceDiscovery

final class ServiceDiscovery {

    static let serviceType = "_display-bridge._tcp"
    static let discoveryTimeout: TimeInterval = 10.0

    weak var delegate: ServiceDiscoveryDelegate?

    private var browser: NWBrowser?
    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "com.display-bridge.discovery")
    private var resolved = false
    private var timeoutWork: DispatchWorkItem?

    // MARK: - Public API

    /// Start browsing for display-bridge senders on the local network.
    func startBrowsing() {
        guard browser == nil else { return }
        resolved = false

        log("Browsing for \(ServiceDiscovery.serviceType) services...")

        let params = NWParameters()
        params.includePeerToPeer = true

        let descriptor = NWBrowser.Descriptor.bonjour(
            type: ServiceDiscovery.serviceType,
            domain: "local."
        )

        let b = NWBrowser(for: descriptor, using: params)
        b.stateUpdateHandler = { [weak self] state in
            self?.handleBrowserState(state)
        }
        b.browseResultsChangedHandler = { [weak self] results, changes in
            self?.handleBrowseResults(results, changes: changes)
        }
        browser = b
        b.start(queue: queue)

        // Set a timeout — fall back to hardcoded IP if no service found
        let work = DispatchWorkItem { [weak self] in
            guard let self = self, !self.resolved else { return }
            self.log("Discovery timed out after \(ServiceDiscovery.discoveryTimeout)s")
            self.stopBrowsing()
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.delegate?.serviceDiscoveryDidFail(self)
            }
        }
        timeoutWork = work
        queue.asyncAfter(deadline: .now() + ServiceDiscovery.discoveryTimeout,
                         execute: work)
    }

    /// Stop browsing.
    func stopBrowsing() {
        timeoutWork?.cancel()
        timeoutWork = nil
        browser?.cancel()
        browser = nil
        connection?.cancel()
        connection = nil
    }

    // MARK: - Browser State

    private func handleBrowserState(_ state: NWBrowser.State) {
        switch state {
        case .ready:
            log("Browser ready")
        case .failed(let error):
            log("Browser failed: \(error)")
            stopBrowsing()
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.delegate?.serviceDiscoveryDidFail(self)
            }
        case .cancelled:
            log("Browser cancelled")
        default:
            break
        }
    }

    // MARK: - Browse Results

    private func handleBrowseResults(_ results: Set<NWBrowser.Result>,
                                      changes: Set<NWBrowser.Result.Change>) {
        guard !resolved else { return }

        for change in changes {
            switch change {
            case .added(let result):
                log("Found service: \(result.endpoint)")
                resolveEndpoint(result.endpoint)
                return  // resolve the first one found
            case .removed(let result):
                log("Service removed: \(result.endpoint)")
            default:
                break
            }
        }
    }

    // MARK: - Endpoint Resolution

    /// Resolve a Bonjour endpoint to an IP address by establishing
    /// a lightweight NWConnection.
    private func resolveEndpoint(_ endpoint: NWEndpoint) {
        guard !resolved else { return }

        // Create a UDP connection to resolve the endpoint to an IP.
        // Force IPv4 (A record) so we never get an IPv6 address that may be
        // blocked by WiFi client isolation.
        let params = NWParameters.udp
        if let ipOptions = params.defaultProtocolStack.internetProtocol
                            as? NWProtocolIP.Options {
            ipOptions.version = .v4
        }
        let conn = NWConnection(to: endpoint, using: params)
        connection = conn

        conn.stateUpdateHandler = { [weak self] state in
            guard let self = self, !self.resolved else { return }

            switch state {
            case .ready:
                // Extract the resolved IP from the current path
                if let path = conn.currentPath,
                   let remoteEndpoint = path.remoteEndpoint {
                    self.extractAddress(from: remoteEndpoint)
                } else {
                    self.log("Connection ready but no remote endpoint")
                }
                conn.cancel()

            case .failed(let error):
                self.log("Resolution failed: \(error)")
                conn.cancel()

            default:
                break
            }
        }

        conn.start(queue: queue)

        // Also try to extract from the endpoint directly if it's a hostPort
        if case .hostPort(let host, let port) = endpoint {
            // For .ipv4/.ipv6, use inet_ntop for a clean string (no zone ID)
            switch host {
            case .ipv4(let addr):
                let raw = addr.rawValue
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                guard raw.withUnsafeBytes({ ptr in
                    inet_ntop(AF_INET, ptr.baseAddress, &buf, socklen_t(INET_ADDRSTRLEN))
                }) != nil else { return }
                let ipStr = String(cString: buf)
                log("Resolved endpoint: \(ipStr):\(port.rawValue)")
                resolved = true
                timeoutWork?.cancel()
                stopBrowsingKeepResult()
                DispatchQueue.main.async { [weak self] in
                    guard let self = self else { return }
                    self.delegate?.serviceDiscovery(self,
                                                    didFindSender: ipStr,
                                                    port: port.rawValue)
                }
            default:
                // For .name and .ipv6, fall through to NWConnection resolution
                break
            }
        }
    }

    private func extractAddress(from endpoint: NWEndpoint) {
        guard !resolved else { return }

        if case .hostPort(let host, let port) = endpoint {
            let hostStr: String
            switch host {
            case .ipv4(let addr):
                // Use inet_ntop on raw bytes to get a clean string
                let raw = addr.rawValue  // 4 bytes
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                guard raw.withUnsafeBytes({ ptr in
                    inet_ntop(AF_INET, ptr.baseAddress, &buf, socklen_t(INET_ADDRSTRLEN))
                }) != nil else { return }
                hostStr = String(cString: buf)
            case .ipv6(let addr):
                // Use inet_ntop on raw bytes — avoids zone ID in string description
                let raw = addr.rawValue  // 16 bytes
                var buf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
                guard raw.withUnsafeBytes({ ptr in
                    inet_ntop(AF_INET6, ptr.baseAddress, &buf, socklen_t(INET6_ADDRSTRLEN))
                }) != nil else { return }
                let s = String(cString: buf)
                // Skip link-local (fe80::) — not routable across subnets
                if s.hasPrefix("fe80") { return }
                hostStr = s
            case .name(let name, _):
                // Strip zone ID just in case (e.g. "hostname%en0")
                if let pct = name.firstIndex(of: "%") {
                    hostStr = String(name[name.startIndex..<pct])
                } else {
                    hostStr = name
                }
            @unknown default:
                return
            }

            resolved = true
            timeoutWork?.cancel()
            log("Resolved sender: \(hostStr):\(port.rawValue)")

            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.delegate?.serviceDiscovery(self,
                                                didFindSender: hostStr,
                                                port: port.rawValue)
            }
        }
    }

    private func stopBrowsingKeepResult() {
        browser?.cancel()
        browser = nil
    }

    // MARK: - Logging

    private func log(_ msg: String) {
        print("[discovery] \(msg)")
    }
}
