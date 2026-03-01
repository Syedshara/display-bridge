#!/bin/bash
# start-sender.sh — Launch the display-bridge sender on Ubuntu.
#
# Usage:
#   bash scripts/start-sender.sh [OPTIONS]
#
# Options:
#   -b BITRATE   Bitrate in kbps (default: 30000)
#   -r WxH       Resolution (default: 3072x1920)
#   -f FPS       Framerate (default: 60)
#   -p PORT      SRT listen port (default: 5000)
#   -l LATENCY   SRT latency in ms (default: 20)
#   -v           Verbose logging
#   -h           Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/ubuntu/build"
BINARY="$BUILD_DIR/db-sender"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${GREEN}[sender]${NC} $1"; }
warn() { echo -e "${YELLOW}[sender]${NC} $1"; }
err()  { echo -e "${RED}[sender]${NC} $1" >&2; }
info() { echo -e "${CYAN}[sender]${NC} $1"; }

show_help() {
    echo "display-bridge sender launcher"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -b BITRATE   Bitrate in kbps (default: 30000)"
    echo "  -r WxH       Resolution (default: 3072x1920)"
    echo "  -f FPS       Framerate (default: 60)"
    echo "  -p PORT      SRT listen port (default: 5000)"
    echo "  -l LATENCY   SRT latency in ms (default: 20)"
    echo "  -v           Verbose logging"
    echo "  -h           Show this help"
    echo ""
    echo "Example:"
    echo "  $0 -b 20000 -l 30"
}

# --- Parse options ---
VERBOSE=""
while getopts "b:r:f:p:l:vh" opt; do
    case $opt in
        b) export DB_BITRATE_KBPS="$OPTARG" ;;
        r) export DB_RESOLUTION="$OPTARG" ;;
        f) export DB_FPS="$OPTARG" ;;
        p) export DB_VIDEO_PORT="$OPTARG" ;;
        l) export DB_SRT_LATENCY="$OPTARG" ;;
        v) VERBOSE=1 ;;
        h) show_help; exit 0 ;;
        *) show_help; exit 1 ;;
    esac
done

# --- Pre-flight checks ---

# 1. Check binary exists
if [[ ! -x "$BINARY" ]]; then
    warn "Binary not found at $BINARY"
    log "Building from source..."
    cd "$PROJECT_ROOT/ubuntu"
    if [[ ! -d build ]]; then
        meson setup build
    fi
    meson compile -C build
    if [[ ! -x "$BINARY" ]]; then
        err "Build failed — binary not found"
        exit 1
    fi
fi

# 2. Check VAAPI driver
if ! vainfo 2>/dev/null | grep -q "VAEntrypointEncSlice"; then
    warn "VAAPI encode entrypoint not found — hardware encoding may fail"
fi

# 3. Check PipeWire is running
if ! pgrep -x pipewire &>/dev/null; then
    err "PipeWire is not running. Start it first."
    exit 1
fi

# 4. Check xdg-desktop-portal is running (needed for ScreenCast)
if ! pgrep -f "xdg-desktop-portal" &>/dev/null; then
    warn "xdg-desktop-portal not detected — screen capture may fail"
fi

# 5. Show local IP addresses for reference
log "Local IP addresses:"
ip -4 addr show | grep -oP 'inet \K[\d.]+' | grep -v '127.0.0.1' | while read -r ip; do
    info "  $ip"
done

# 6. Check if vkms module is loaded (for virtual display)
if lsmod | grep -q vkms; then
    info "vkms virtual display module loaded"
fi

# --- Launch ---
log "Starting display-bridge sender..."
info "Binary: $BINARY"
info "SRT port: ${DB_VIDEO_PORT:-5000}"
info "Input port: 5001"
info "Press Ctrl+C to stop"
echo ""

# Ensure VAAPI uses the Intel iHD driver
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=/usr/lib/x86_64-linux-gnu/dri

# Run with optional stdbuf for line-buffered output
if [[ -n "$VERBOSE" ]]; then
    exec stdbuf -oL "$BINARY" 2>&1
else
    exec "$BINARY" 2>&1
fi
