import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            Self.positionMainWindow()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    private static func positionMainWindow() {
        guard let window = NSApp.windows.first(where: { $0.canBecomeMain && $0.isVisible })
                ?? NSApp.windows.first(where: { $0.canBecomeMain }),
              let screen = window.screen ?? NSScreen.main else {
            return
        }
        let visible = screen.visibleFrame
        var frame = window.frame
        frame.origin.x = visible.midX - frame.width / 2
        frame.origin.y = visible.maxY - frame.height - 20
        window.setFrame(frame, display: true)
    }
}

@main
struct EarCueApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AudioModel()

    var body: some Scene {
        WindowGroup("EarCue", id: "main") {
            ContentView()
                .environmentObject(model)
                .task {
                    if ProcessInfo.processInfo.arguments.contains("--autostart") {
                        model.start()
                    }
                }
        }
        .defaultSize(width: 820, height: 760)
        .windowResizability(.contentSize)

        MenuBarExtra {
            MenuBarPanelView()
                .environmentObject(model)
        } label: {
            Image(systemName: model.isRunning ? "mic.fill" : "mic")
        }
        .menuBarExtraStyle(.window)
    }
}
