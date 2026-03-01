mog@MOGs-MacBook-Pro mac % swift build -c release  
Building for production...
/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/ServiceDiscovery.swift:175:21: warning: variable 'raw' was never mutated; consider changing to 'let' constant
173 | switch host {
174 | case .ipv4(let addr):
175 | var raw = addr.rawValue
| `- warning: variable 'raw' was never mutated; consider changing to 'let' constant
176 | var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
177 | guard raw.withUnsafeBytes({ ptr in

/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/ServiceDiscovery.swift:206:21: warning: variable 'raw' was never mutated; consider changing to 'let' constant
204 | case .ipv4(let addr):
205 | // Use inet_ntop on raw bytes to get a clean string
206 | var raw = addr.rawValue // 4 bytes
| `- warning: variable 'raw' was never mutated; consider changing to 'let' constant
207 | var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
208 | guard raw.withUnsafeBytes({ ptr in

/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/ServiceDiscovery.swift:214:21: warning: variable 'raw' was never mutated; consider changing to 'let' constant
212 | case .ipv6(let addr):
213 | // Use inet_ntop on raw bytes — avoids zone ID in string description
214 | var raw = addr.rawValue // 16 bytes
| `- warning: variable 'raw' was never mutated; consider changing to 'let' constant
215 | var buf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
216 | guard raw.withUnsafeBytes({ ptr in
ld: warning: building for macOS-13.0, but linking with dylib '/usr/local/opt/srt/lib/libsrt.1.5.dylib' which was built for newer version 14.0
[5/5] Linking DisplayBridgeReceiver
Build complete! (5.74s)
mog@MOGs-MacBook-Pro mac % .build/release/DisplayBridgeReceiver
[app] launching DisplayBridgeReceiver
[renderer] initialized (device: AMD Radeon Pro 555X)
[794058859.160] [srt-recv] SRT library started (version 1.5.4)
[input] UDP socket ready -> 10.135.95.11:5001
[discovery] Browsing for \_display-bridge.\_tcp services...
[discovery] Browser ready
[vc] searching for Ubuntu sender via mDNS...
[discovery] Found service: display-bridge-sender.\_display-bridge.\_tcp.local.
[input] event monitoring started
[discovery] Resolved sender: 2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d:5000
[app] window created (1680x1050), entering full-screen
[vc] mDNS discovered sender at 2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d:5000
[vc] target host: 2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d
[input] destination updated -> 2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d:5001
[vc] pipeline started, waiting for SRT connection...
[794058862.215] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058866.226] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058870.240] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058874.248] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[794058878.250] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058882.254] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058886.258] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058890.262] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=0, decoded=0
[794058894.265] [srt-recv] connect to [2401:4900:9151:afb3:eb4d:acab:fcd0:3c3d]:5000 failed: Connection setup failure: connection timed out
^C
mog@MOGs-MacBook-Pro mac %
