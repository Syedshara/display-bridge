#!/bin/bash
# setup-mac.sh — Run this ON THE MAC to build DisplayBridgeReceiver.
#
# Prerequisites:
#   1. Homebrew installed
#   2. Xcode Command Line Tools (xcode-select --install)
#   3. The mac/ directory transferred to the Mac
#
# Usage:
#   cd /path/to/display-bridge
#   bash scripts/setup-mac.sh [UBUNTU_IP]
#
# If UBUNTU_IP is provided, it replaces the placeholder in SRTReceiver.swift.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log()  { echo -e "${GREEN}[setup]${NC} $1"; }
warn() { echo -e "${YELLOW}[setup]${NC} $1"; }
err()  { echo -e "${RED}[setup]${NC} $1" >&2; }

# Determine script directory and project root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MAC_DIR="$PROJECT_ROOT/mac"

# --- Check we're on macOS ---
if [[ "$(uname)" != "Darwin" ]]; then
    err "This script must be run on macOS, not $(uname)."
    exit 1
fi

# --- Check macOS version ---
MACOS_VER=$(sw_vers -productVersion)
log "macOS version: $MACOS_VER"

# --- Check for Xcode Command Line Tools ---
if ! xcode-select -p &>/dev/null; then
    warn "Xcode Command Line Tools not found. Installing..."
    xcode-select --install
    echo "Please wait for the installation to complete, then re-run this script."
    exit 1
fi
log "Xcode Command Line Tools: $(xcode-select -p)"

# --- Check for Homebrew ---
if ! command -v brew &>/dev/null; then
    err "Homebrew not found. Install it from https://brew.sh/"
    exit 1
fi
log "Homebrew: $(brew --version | head -1)"

# --- Install SRT via brew ---
if brew list srt &>/dev/null; then
    log "libsrt already installed: $(brew info srt --json | python3 -c 'import sys,json; print(json.load(sys.stdin)[0]["versions"]["stable"])' 2>/dev/null || echo 'unknown')"
else
    log "Installing libsrt via Homebrew..."
    brew install srt
fi

# --- Verify pkg-config can find SRT ---
if ! command -v pkg-config &>/dev/null; then
    warn "pkg-config not found, installing..."
    brew install pkg-config
fi

if pkg-config --exists srt; then
    log "pkg-config srt: $(pkg-config --modversion srt)"
else
    err "pkg-config cannot find srt. Check brew installation."
    exit 1
fi

# --- Detect Intel vs Apple Silicon paths ---
ARCH=$(uname -m)
if [[ "$ARCH" == "x86_64" ]]; then
    BREW_PREFIX="/usr/local"
    log "Architecture: Intel x86_64 (brew prefix: $BREW_PREFIX)"
elif [[ "$ARCH" == "arm64" ]]; then
    BREW_PREFIX="/opt/homebrew"
    log "Architecture: Apple Silicon arm64 (brew prefix: $BREW_PREFIX)"
else
    warn "Unknown architecture: $ARCH — assuming /usr/local"
    BREW_PREFIX="/usr/local"
fi

# --- Set Ubuntu IP if provided ---
UBUNTU_IP="${1:-}"
if [[ -n "$UBUNTU_IP" ]]; then
    SRT_FILE="$MAC_DIR/Sources/DisplayBridgeReceiver/SRTReceiver.swift"
    if [[ -f "$SRT_FILE" ]]; then
        log "Setting Ubuntu IP to $UBUNTU_IP in SRTReceiver.swift"
        sed -i '' "s|static let UBUNTU_IP = \"192.168.1.100\"|static let UBUNTU_IP = \"$UBUNTU_IP\"|" "$SRT_FILE"
    else
        warn "SRTReceiver.swift not found at expected path"
    fi
else
    warn "No Ubuntu IP provided. Edit SRTReceiver.swift manually before running."
    warn "Usage: $0 192.168.1.XXX"
fi

# --- Build ---
log "Building DisplayBridgeReceiver..."
cd "$MAC_DIR"

# Set pkg-config path for SPM to find SRT
export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

swift build -c release 2>&1 | tee /tmp/db-mac-build.log
BUILD_STATUS=${PIPESTATUS[0]}

if [[ $BUILD_STATUS -eq 0 ]]; then
    BINARY="$MAC_DIR/.build/release/DisplayBridgeReceiver"
    log "Build SUCCESS!"
    log "Binary: $BINARY"
    log ""
    log "To run:"
    log "  $BINARY"
    log ""
    log "Make sure the Ubuntu sender (db-sender) is running first!"
else
    err "Build FAILED. See /tmp/db-mac-build.log for details."
    exit 1
fi
