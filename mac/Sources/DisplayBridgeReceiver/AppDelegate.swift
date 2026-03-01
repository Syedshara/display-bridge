/*
 * AppDelegate.swift
 * Creates the application window programmatically and enters full-screen.
 *
 * No .xib or .storyboard — everything is code. The window hosts a
 * ViewController whose view is the MetalRenderer's MTKView.
 */

import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {

    var window: NSWindow?
    var viewController: ViewController?

    // MARK: - NSApplicationDelegate

    func applicationDidFinishLaunching(_ notification: Notification) {
        print("[app] launching DisplayBridgeReceiver")

        // Activate the app (bring to front)
        NSApp.setActivationPolicy(.regular)

        // Create the view controller (owns the full pipeline)
        let vc = ViewController()
        self.viewController = vc

        // Determine window frame from the main screen
        let screen = NSScreen.main ?? NSScreen.screens.first!
        let frame = screen.frame

        // Create window
        let win = NSWindow(
            contentRect: frame,
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false,
            screen: screen
        )
        win.contentViewController = vc
        win.title = "Display Bridge"
        win.isReleasedWhenClosed = false
        win.acceptsMouseMovedEvents = true
        win.titlebarAppearsTransparent = true
        win.titleVisibility = .hidden
        win.backgroundColor = .black

        // Show window
        win.makeKeyAndOrderFront(nil)
        win.makeFirstResponder(vc.metalRenderer?.view)

        self.window = win

        // Enter full-screen after a brief delay (lets the window finish layout)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak win] in
            guard let win = win else { return }
            if !win.styleMask.contains(.fullScreen) {
                win.toggleFullScreen(nil)
            }
        }

        // Activate app
        NSApp.activate(ignoringOtherApps: true)
        print("[app] window created (\(Int(frame.width))x\(Int(frame.height))), entering full-screen")
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    func applicationWillTerminate(_ notification: Notification) {
        viewController?.shutdown()
        print("[app] terminated")
    }
}
