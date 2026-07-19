import SwiftUI

@main
struct TrackNRaceMinimalRecorderApp: App {
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
