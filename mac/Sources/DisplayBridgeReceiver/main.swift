/*
 * main.swift
 * Entry point for DisplayBridgeReceiver.
 * Starts NSApplication manually (no .xib / .storyboard).
 */

import AppKit

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
