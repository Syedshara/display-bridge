Build complete! (0.20s)
mog@MOGs-MacBook-Pro mac % .build/release/DisplayBridgeReceiver
[app] launching DisplayBridgeReceiver
[renderer] initialized (device: AMD Radeon Pro 555X)
[794063885.418] [srt-recv] SRT library started (version 1.5.4)
[input] UDP socket ready -> 10.135.95.11:5001
[discovery] Browsing for \_display-bridge.\_tcp services...
[discovery] Browser ready
[vc] searching for Ubuntu sender via mDNS...
[discovery] Found service: display-bridge-sender.\_display-bridge.\_tcp.local.
[input] event monitoring started
[discovery] Resolved sender: 10.135.95.11:5000
[app] window created (1680x1050), entering full-screen
[vc] mDNS discovered sender at 10.135.95.11:5000
[vc] target host: 10.135.95.11
[input] destination updated -> 10.135.95.11:5001
[vc] pipeline started, waiting for SRT connection...
[794063886.597] [srt-recv] connected to 10.135.95.11:5000
[vc] SRT connected -- receiving video
[vc] stats: received=0, decoded=0
[decoder] format description created: 2880x1800 HEVC
[decoder] decompression session created
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
[vc] stats: received=1, decoded=1
18:48:54.859296/!W:SRT.br: CRcvBuffer.readMessage(): nothing to read. Ignored isRcvDataReady() result?
[794063934.861] [srt-recv] connection lost (err=2001), reconnecting...
[vc] SRT disconnected -- waiting for reconnect...
[vc] stats: received=1, decoded=1
[794063937.864] [srt-recv] connect to 10.135.95.11:5000 failed: Connection setup failure: connection timed out
[vc] stats: received=1, decoded=1
