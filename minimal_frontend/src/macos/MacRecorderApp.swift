import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

@main
struct TrackNRaceMinimalRecorderApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = RecorderViewModel()

    var body: some Scene {
        WindowGroup("Track N Race Minimal Recorder") {
            ContentView(model: model)
        }
        .defaultSize(width: 620, height: 286)
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About Track N Race Minimal Recorder") {
                    model.showsAttributions = true
                }
            }
        }
    }
}
