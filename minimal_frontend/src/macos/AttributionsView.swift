import AppKit
import SwiftUI

struct AttributionsView: View {
    let attributions: [TNRAttribution]
    @Environment(\.dismiss) private var dismiss
    @State private var selectedAttribution: TNRAttribution?

    private var licenseIsPresented: Binding<Bool> {
        Binding(
            get: { selectedAttribution != nil },
            set: { if !$0 { selectedAttribution = nil } }
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if let project = attributions.first {
                VStack(alignment: .leading, spacing: 5) {
                    Text(project.name)
                        .font(.title2.weight(.semibold))
                    Text("Version \(project.version)")
                    Text("Lightweight background telemetry recorder for F1 sim racing.")
                        .foregroundStyle(.secondary)
                    Text(project.copyrightText)
                    if let url = URL(string: project.website) {
                        Link(project.website, destination: url)
                    }
                    Button("View GPL v3 license") { selectedAttribution = project }
                }
            }

            Divider()
            Text("Third-party software")
                .font(.headline)

            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(attributions.dropFirst().enumerated()), id: \.offset) { index, item in
                        attributionRow(item)
                        if index < attributions.count - 2 { Divider() }
                    }
                }
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(.separator))

            HStack {
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(22)
        .frame(width: 720, height: 520)
        .sheet(isPresented: licenseIsPresented) {
            if let item = selectedAttribution {
                LicenseView(attribution: item)
            }
        }
    }

    private func attributionRow(_ item: TNRAttribution) -> some View {
        HStack(alignment: .top, spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Text(item.name).fontWeight(.semibold)
                Text("Version \(item.version) · \(item.licenseName)")
                    .foregroundStyle(.secondary)
                Text(item.copyrightText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if let url = URL(string: item.website) {
                    Link(item.website, destination: url)
                        .font(.caption)
                }
            }
            Spacer()
            Button("View License") { selectedAttribution = item }
        }
        .padding(12)
    }
}

private struct LicenseView: View {
    let attribution: TNRAttribution
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("\(attribution.name) — \(attribution.licenseName)")
                .font(.headline)
            TextEditor(text: .constant(attribution.licenseText))
                .font(.system(.body, design: .monospaced))
                .border(Color(nsColor: .separatorColor))
            HStack {
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(attribution.licenseText, forType: .string)
                }
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(18)
        .frame(width: 680, height: 560)
    }
}
