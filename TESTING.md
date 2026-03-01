# Display Bridge — Integration Test Checklist

Step-by-step procedure for the first end-to-end test between Ubuntu and Mac.

## Pre-flight (Ubuntu)

- [ ] **1.** Verify VAAPI is working:
  ```bash
  vainfo --display drm --device /dev/dri/renderD128 2>&1 | grep -i hevc
  ```
  Expect: `VAEntrypointEncSlice` for HEVC

- [ ] **2.** Run unit tests:
  ```bash
  cd ~/Documents/git/display-bridge/ubuntu
  ./build/test_encode_only    # expect: 5 frames, PASS
  ./build/test_loopback       # expect: 30/30 frames, 0 errors, PASS
  ```

- [ ] **3.** Verify PipeWire is running:
  ```bash
  pw-cli info | head -3
  ```

- [ ] **4.** Note your WiFi IP:
  ```bash
  ip -4 addr show wlp0s20f3 | grep inet
  ```

- [ ] **5.** Ensure ports 5000 and 5001 are open:
  ```bash
  sudo ufw allow 5000/udp
  sudo ufw allow 5001/udp
  ```

## Pre-flight (Mac)

- [ ] **6.** Verify SRT is installed:
  ```bash
  pkg-config --modversion srt    # expect: 1.5.x
  ```

- [ ] **7.** Build the receiver:
  ```bash
  cd ~/Documents/git/display-bridge/mac
  swift build
  ```
  Expect: clean build, no errors

- [ ] **8.** If mDNS won't work, set the Ubuntu IP:
  Edit `SRTReceiver.swift` line 34:
  ```swift
  static let DEFAULT_UBUNTU_IP = "YOUR_UBUNTU_IP"
  ```
  Then rebuild: `swift build`

## Test 1: Connection

- [ ] **9.** Start the sender on Ubuntu:
  ```bash
  cd ~/Documents/git/display-bridge
  LIBVA_DRIVER_NAME=iHD ./ubuntu/build/db-sender
  ```
  Expect:
  ```
  [main] Initializing VAAPI encoder...
  [main] Initializing PipeWire capture...
  [main] Initializing SRT streamer...
  [streamer] SRT listener ready on port 5000
  [main] Starting mDNS service advertisement...
  [main] Waiting for Mac receiver on SRT port 5000...
  ```

- [ ] **10.** A PipeWire ScreenCast portal dialog will appear — **grant screen sharing permission**.

- [ ] **11.** Start the receiver on Mac:
  ```bash
  cd ~/Documents/git/display-bridge/mac
  swift run
  ```
  Expect:
  ```
  [discovery] Browsing for _display-bridge._tcp services...
  [discovery] Found service: ...
  [vc] mDNS discovered sender at X.X.X.X:5000
  [srt-recv] connected to X.X.X.X:5000
  [vc] SRT connected -- receiving video
  ```

- [ ] **12.** On Ubuntu, expect:
  ```
  [streamer] Receiver connected from X.X.X.X:YYYYY
  [main] Receiver connected — streaming
  ```

## Test 2: Video

- [ ] **13.** Mac should enter full-screen mode and display the Ubuntu desktop.

- [ ] **14.** Check for smooth rendering (no tearing, no artifacts). Move windows on Ubuntu to verify real-time update.

- [ ] **15.** Check stats output on Mac (every 5 seconds):
  ```
  [vc] stats: received=NNN, decoded=NNN
  ```
  Both numbers should be close and increasing steadily (~60/sec for 60fps).

## Test 3: Input

- [ ] **16.** Move the mouse on the Mac receiver window. On Ubuntu, the cursor should move.

- [ ] **17.** Type on the Mac keyboard. On Ubuntu, the keystrokes should appear.

- [ ] **18.** Test left-click, right-click, scroll wheel.

## Test 4: Resilience

- [ ] **19.** Disconnect WiFi briefly on the Mac, then reconnect.
  Expect: receiver reconnects automatically, video resumes.

- [ ] **20.** Kill the receiver (Cmd+Q), restart it.
  Expect: sender logs "Receiver disconnected", then "Receiver connected" when receiver restarts.

- [ ] **21.** Check adaptive bitrate by monitoring sender logs for bitrate adjustments:
  ```
  [main] WARN: Congestion detected (loss=..., rtt=...) -> XXXXX kbps
  ```
  (Only appears under network stress.)

## Test 5: Heartbeat

- [ ] **22.** With receiver connected but no screen changes on Ubuntu, verify the connection stays alive. After 60+ seconds of idle, the connection should NOT drop (heartbeat keeps it alive).

## Performance Benchmarks (Optional)

- [ ] **23.** Measure end-to-end latency: display a millisecond timer on Ubuntu, photograph both screens simultaneously. Target: <50ms.

- [ ] **24.** Monitor CPU usage on both machines during streaming. Ubuntu encoder should be mostly GPU-bound (low CPU). Mac decoder should use VideoToolbox hardware.

## Sign-off

| Test               | Pass/Fail | Notes |
|--------------------|-----------|-------|
| Connection         |           |       |
| Video rendering    |           |       |
| Input forwarding   |           |       |
| Reconnect          |           |       |
| Heartbeat          |           |       |
| Latency (<50ms)    |           |       |
