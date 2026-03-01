#!/bin/bash
# start-receiver.sh — Launch DisplayBridgeReceiver on the Mac.
#
# Usage:
#   bash scripts/start-receiver.sh [UBUNTU_IP]
#
# If UBUNTU_IP is provided, it replaces the placeholder in SRTReceiver.swift
# and rebuilds before launching.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MAC_DIR="$PROJECT_ROOT/mac"
BINARY="$MAC_DIR/.build/release/DisplayBridgeReceiver"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${GREEN}[receiver]${NC} $1"; }
warn() { echo -e "${YELLOW}[receiver]${NC} $1"; }
err()  { echo -e "${RED}[receiver]${NC} $1" >&2; }
info() { echo -e "${CYAN}[receiver]${NC} $1"; }

# --- Check we're on macOS ---
if [[ "$(uname)" != "Darwin" ]]; then
    err "This script must be run on macOS."
    exit 1
fi

# --- Set Ubuntu IP if provided ---
UBUNTU_IP="${1:-}"
NEED_REBUILD=0

if [[ -n "$UBUNTU_IP" ]]; then
    SRT_FILE="$MAC_DIR/Sources/DisplayBridgeReceiver/SRTReceiver.swift"
    if [[ -f "$SRT_FILE" ]]; then
        CURRENT_IP=$(grep 'static let UBUNTU_IP' "$SRT_FILE" | sed 's/.*"\(.*\)".*/\1/')
        if [[ "$CURRENT_IP" != "$UBUNTU_IP" ]]; then
            log "Updating Ubuntu IP: $CURRENT_IP → $UBUNTU_IP"
            sed -i '' "s|static let UBUNTU_IP = \"$CURRENT_IP\"|static let UBUNTU_IP = \"$UBUNTU_IP\"|" "$SRT_FILE"
            NEED_REBUILD=1
        else
            log "Ubuntu IP already set to $UBUNTU_IP"
        fi
    fi
fi

# --- Build if needed ---
if [[ ! -x "$BINARY" ]] || [[ $NEED_REBUILD -eq 1 ]]; then
    log "Building DisplayBridgeReceiver..."

    # Detect brew prefix
    ARCH=$(uname -m)
    if [[ "$ARCH" == "x86_64" ]]; then
        BREW_PREFIX="/usr/local"
    else
        BREW_PREFIX="/opt/homebrew"
    fi
    export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

    cd "$MAC_DIR"
    swift build -c release 2>&1 | tail -5

    if [[ ! -x "$BINARY" ]]; then
        err "Build failed"
        exit 1
    fi
    log "Build successful"
fi

# --- Show connection info ---
log "DisplayBridgeReceiver"
CURRENT_IP=$(grep 'static let UBUNTU_IP' "$MAC_DIR/Sources/DisplayBridgeReceiver/SRTReceiver.swift" 2>/dev/null | sed 's/.*"\(.*\)".*/\1/')
info "Connecting to Ubuntu at: ${CURRENT_IP:-unknown}"
info "SRT port: 5000"
info "Input UDP port: 5001"
info "Window: full-screen"
info "Press Cmd+Q to quit"
echo ""

# --- Launch ---
exec "$BINARY"
