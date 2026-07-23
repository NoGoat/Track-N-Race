import SwiftUI

struct ContentView: View {
    @ObservedObject var model: RecorderViewModel

    private var alertIsPresented: Binding<Bool> {
        Binding(
            get: { model.alertMessage != nil },
            set: { if !$0 { model.alertMessage = nil } }
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            configuration
            Divider()
            liveStatus
            HStack {
                Spacer()
                Button("Attributions…") { model.showsAttributions = true }
            }
        }
        .padding(16)
        .frame(width: 620, height: 286, alignment: .topLeading)
        .sheet(isPresented: $model.showsAttributions) {
            AttributionsView(attributions: model.attributions)
        }
        .alert("Track N Race Minimal Recorder", isPresented: alertIsPresented) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.alertMessage ?? "")
        }
    }

    private var configuration: some View {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 10) {
            GridRow {
                fieldLabel("Recording folder")
                Text(model.outputFolder.isEmpty ? "No folder selected" : model.outputFolder)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .foregroundStyle(model.outputFolder.isEmpty ? .secondary : .primary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button("Browse…") { model.chooseFolder() }
            }

            GridRow {
                fieldLabel("Protocol override")
                Picker("Protocol override", selection: Binding(
                    get: { model.protocolIndex },
                    set: { model.updateProtocol($0) }
                )) {
                    ForEach(RecorderViewModel.protocols) { option in
                        Text(option.title).tag(option.id)
                    }
                }
                .labelsHidden()
                .frame(maxWidth: .infinity, alignment: .leading)
                Color.clear.frame(width: 1, height: 1)
            }

            GridRow {
                fieldLabel("IPv4 bind address")
                TextField("0.0.0.0", text: $model.bindAddress)
                    .textFieldStyle(.roundedBorder)
                Color.clear.frame(width: 1, height: 1)
            }

            GridRow {
                fieldLabel("UDP port")
                TextField("20777", text: $model.portText)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { model.applyNetwork() }
                Button("Apply network") { model.applyNetwork() }
                    .keyboardShortcut(.return, modifiers: [.command])
            }
        }
    }

    private var liveStatus: some View {
        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 8) {
            statusRow("Circuit", model.circuit)
            statusRow("Session", model.session)
            statusRow("Recording status", model.recordingStatus)
            GridRow {
                fieldLabel("Error")
                Text(model.errorText)
                    .foregroundStyle(model.errorText == "None" ? Color.secondary : Color.red)
                    .lineLimit(2)
                    .gridCellColumns(2)
            }
        }
    }

    private func fieldLabel(_ value: String) -> some View {
        Text(value)
            .foregroundStyle(.secondary)
            .frame(width: 125, alignment: .trailing)
    }

    private func statusRow(_ label: String, _ value: String) -> some View {
        GridRow {
            fieldLabel(label)
            Text(value)
                .textSelection(.enabled)
                .gridCellColumns(2)
        }
    }
}
