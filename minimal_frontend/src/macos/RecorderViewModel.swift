import AppKit
import Foundation

@MainActor
final class RecorderViewModel: ObservableObject {
    struct ProtocolOption: Identifiable {
        let id: Int
        let title: String
    }

    static let protocols = [
        ProtocolOption(id: 0, title: "Auto"),
        ProtocolOption(id: 1, title: "F1 24"),
        ProtocolOption(id: 2, title: "F1 25"),
        ProtocolOption(id: 3, title: "F1 26")
    ]

    @Published var outputFolder: String
    @Published var bindAddress: String
    @Published var portText: String
    @Published var protocolIndex: Int
    @Published var circuit = "Unavailable"
    @Published var session = "Unavailable"
    @Published var recordingStatus = "Not recording"
    @Published var errorText = "None"
    @Published var alertMessage: String?
    @Published var showsAttributions = false

    private let bridge: TNRRecorderBridge

    init() {
        let bridge = TNRRecorderBridge(defaults: .standard)
        self.bridge = bridge
        outputFolder = bridge.outputFolder
        bindAddress = bridge.bindAddress
        portText = String(bridge.port)
        protocolIndex = bridge.protocolIndex

        bridge.sessionUpdateHandler = { [weak self] circuit, session in
            self?.circuit = circuit
            self?.session = session
        }
        bridge.recordingUpdateHandler = { [weak self] status, error in
            self?.recordingStatus = status
            self?.errorText = error
        }

        _ = bridge.start()
    }

    var attributions: [TNRAttribution] {
        bridge.attributions()
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.title = "Select recording folder"
        panel.prompt = "Select"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        if !outputFolder.isEmpty {
            panel.directoryURL = URL(fileURLWithPath: outputFolder, isDirectory: true)
        }
        guard panel.runModal() == .OK, let folder = panel.url?.path else { return }

        if let error = bridge.selectOutputFolder(folder) {
            alertMessage = error
            return
        }
        outputFolder = bridge.outputFolder
    }

    func applyNetwork() {
        guard let port = Int(portText), (1...65535).contains(port) else {
            alertMessage = "The UDP port must be between 1 and 65535."
            portText = String(bridge.port)
            return
        }

        if let error = bridge.applyNetworkAddress(bindAddress, port: port) {
            alertMessage = error
            bindAddress = bridge.bindAddress
            portText = String(bridge.port)
            return
        }
        bindAddress = bridge.bindAddress
        portText = String(bridge.port)
    }

    func updateProtocol(_ index: Int) {
        protocolIndex = index
        bridge.setProtocolIndex(index)
    }
}
