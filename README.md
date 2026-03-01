# display-bridge

Use an Ubuntu laptop as a sender and a MacBook Pro as an extended wireless display — over WiFi.

Hardware-accelerated HEVC encoding (Intel VAAPI) on Ubuntu, hardware-accelerated decoding (VideoToolbox) and Metal rendering on Mac. Low-latency streaming via SRT protocol.

## Architecture

```
Ubuntu (Sender)                          Mac (Receiver)
+------------------+    SRT/5000    +-------------------+
| PipeWire Capture |  ----------->  | SRT Receiver      |
| VAAPI HEVC Enc   |    WiFi        | VideoToolbox Dec  |
| SRT Listener     |                | Metal Renderer    |
| Avahi mDNS       |  <-----------  | Input Forwarder   |
| uinput Injection |   UDP/5001     | NWBrowser mDNS    |
+------------------+                +-------------------+
```

- **Video**: Ubuntu captures screen via PipeWire ScreenCast portal, encodes to HEVC with VAAPI, streams via SRT message mode
- **Input**: Mac captures keyboard/mouse events, sends as UDP datagrams back to Ubuntu, which injects them via uinput
- **Discovery**: Ubuntu advertises `_display-bridge._tcp` via Avahi; Mac discovers via NWBrowser (Bonjour)

## Specs

| Parameter     | Value                           |
|---------------|---------------------------------|
| Resolution    | 3072x1920 (MacBook Pro 16")     |
| Frame rate    | 60 fps                          |
| Codec         | HEVC Main (H.265)               |
| Bitrate       | 30 Mbps (adaptive: 5-30 Mbps)   |
| SRT latency   | 20 ms                           |
| Transport     | SRT (FILE + MESSAGE API)        |
| Input         | Raw UDP, port 5001              |

## Requirements

### Ubuntu (Sender)
- Ubuntu 24.04 LTS, Wayland/GNOME
- Intel GPU with VAAPI support (tested: Arrow Lake-U)
- Intel VAAPI driver v26+ (from Intel Graphics PPA)
- PipeWire 1.0+
- libsrt 1.5+
- Avahi (avahi-daemon + libavahi-client)
- Build tools: meson, ninja, pkg-config, gcc

### Mac (Receiver)
- macOS 13+ (Ventura or later; tested: Sequoia 15.7.4)
- Intel Mac (tested: MacBook Pro 16" Intel, 3072x1920)
- Homebrew with `srt` package
- Swift 5.9+ (Xcode Command Line Tools)

### Network
- Both machines on the same WiFi network
- 5 GHz WiFi strongly recommended for 30 Mbps throughput
- Ports: 5000/UDP (SRT video), 5001/UDP (input events)

## Quick Start

### 1. Build the Ubuntu Sender

```bash
# Install dependencies (if not already done)
sudo apt install meson ninja-build pkg-config gcc \
    libva-dev libva-drm2 libdrm-dev \
    libpipewire-0.3-dev libsrt-openssl-dev \
    libavahi-client-dev libglib2.0-dev

# Build
cd ubuntu
meson setup build
ninja -C build

# Verify
./build/test_encode_only    # should print 5 frames encoded
./build/test_loopback       # should print 30/30 frames, 0 errors
```

### 2. Build the Mac Receiver

```bash
# Install SRT library
brew install srt

# Verify pkg-config can find it
pkg-config --cflags --libs srt

# Build
cd mac
swift build

# The binary will be at:
# .build/debug/DisplayBridgeReceiver
```

Or use the setup script:
```bash
./scripts/setup-mac.sh
```

### 3. Run

**On Ubuntu:**
```bash
./scripts/start-sender.sh
# Or directly:
LIBVA_DRIVER_NAME=iHD ./ubuntu/build/db-sender
```

**On Mac:**
```bash
./scripts/start-receiver.sh
# Or directly:
cd mac && swift run
```

The Mac receiver will:
1. Search for the Ubuntu sender via mDNS (10 second timeout)
2. Fall back to hardcoded IP if mDNS fails
3. Connect via SRT and begin receiving video
4. Enter full-screen mode automatically
5. Forward keyboard and mouse events back to Ubuntu

### 4. Exit

- Press `Cmd+Q` on the Mac to quit the receiver
- Press `Ctrl+C` on Ubuntu to stop the sender

## Manual IP Configuration

If mDNS discovery doesn't work on your network:

1. Find the Ubuntu machine's WiFi IP: `ip addr show wlp0s20f3`
2. Edit `mac/Sources/DisplayBridgeReceiver/SRTReceiver.swift`:
   ```swift
   static let DEFAULT_UBUNTU_IP = "192.168.1.XXX"  // your Ubuntu IP
   ```
3. Rebuild: `cd mac && swift build`

## Project Structure

```
display-bridge/
├── shared/protocol.h           # Wire protocol (C header, mirrored in Swift)
├── ubuntu/
│   ├── include/                # C headers (encoder, capture, streamer, input, discovery)
│   ├── src/                    # C implementation files
│   ├── meson.build             # Build config (3 targets)
│   └── build/                  # Compiled binaries
├── mac/
│   ├── Package.swift           # SPM manifest
│   ├── Sources/CSRT/           # C wrapper for libsrt
│   ├── Sources/DisplayBridgeReceiver/
│   │   ├── main.swift          # Entry point
│   │   ├── AppDelegate.swift   # Window management
│   │   ├── ViewController.swift # Pipeline orchestration
│   │   ├── Protocol.swift      # Wire protocol (Swift)
│   │   ├── SRTReceiver.swift   # SRT connection + recv loop
│   │   ├── HEVCDecoder.swift   # VideoToolbox HEVC decoding
│   │   ├── MetalRenderer.swift # Metal NV12 rendering
│   │   ├── InputForwarder.swift # Keyboard/mouse event forwarding
│   │   └── ServiceDiscovery.swift # Bonjour mDNS discovery
│   └── Resources/Info.plist    # App metadata
├── scripts/
│   ├── setup-mac.sh            # Mac setup + build
│   ├── start-sender.sh         # Ubuntu launch script
│   └── start-receiver.sh       # Mac launch script
└── tests/                      # Verification tests
```

## Wire Protocol

Each SRT message contains exactly one frame:

```
[db_packet_header_t: 16 bytes] [db_video_header_t: 16 bytes] [HEVC bitstream]
```

- **IDR frames**: VPS + SPS + PPS + IDR slice NALUs (Annex B, 00 00 00 01 start codes)
- **P-frames**: TRAIL_R slice NALU
- **Heartbeats**: packet header only (type=0x04), no payload

Input events (Mac -> Ubuntu, UDP port 5001):
```
[db_packet_header_t: 16 bytes] [db_input_event_t: 8 bytes]
```

## Adaptive Bitrate

The sender monitors SRT connection stats every 2 seconds:
- **Congestion** (packet loss > 10 or RTT > 50ms): bitrate reduced by 20%, minimum 5 Mbps
- **Healthy** (zero loss, RTT < 20ms): bitrate increased by 10%, up to 30 Mbps

## Troubleshooting

### VAAPI not found
```bash
# Check driver
vainfo --display drm --device /dev/dri/renderD128
# Should show iHD driver and HEVC encode entrypoints
```

### PipeWire portal fails
```bash
# Check PipeWire is running
pw-cli info
# Check ScreenCast portal
busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop
```

### SRT connection refused
- Verify both machines are on the same network segment
- Check firewall: `sudo ufw allow 5000/udp && sudo ufw allow 5001/udp`
- Test connectivity: `ping <ubuntu-ip>` from Mac

### Mac receiver shows black screen
- Check that the sender is actually capturing (look for "Encode" log messages)
- Verify the HEVC bitstream is valid (test_loopback should pass)
- Check for VideoToolbox errors in receiver console output

### High latency
- Switch to 5 GHz WiFi
- Ensure no other heavy network traffic
- The adaptive bitrate will automatically reduce quality under congestion
