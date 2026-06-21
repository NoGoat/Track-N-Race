#pragma once

#include <optional>
#include <QString>
#include <QColor>
#include <nlohmann/json.hpp>

// Resolved content for one toast. MainWindow::showToast() feeds this into the
// vendored Toast widget (third_party/qt-toast). Kept Qt-widget-free so the
// mapping below stays pure data-in/data-out.
struct ToastSpec {
    QString label;               // headline (e.g. "Red Flag")
    QString sub;                 // optional secondary line (driver / lap time / reason)
    QColor  color;               // accent + headline colour (per event severity)
    bool    persistent;          // occupies the single persistent slot; no auto-dismiss (duration=0)
    bool    dismissesPersistent; // evicts the current persistent toast before showing self
};

// Pure mapping from telemetry rows to a toast, mirroring the Electron app's
// buildBanner() (src/renderer/src/App.tsx:161).

// race_event row → toast, or nullopt for codes that shouldn't notify (SCAR is
// driven by the session packet; OVTK/SPTP are intentionally silent). `participants`
// is the latest "participants" row (with its "drivers" array) for name lookup.
std::optional<ToastSpec> buildToast(const nlohmann::json& event,
                                    const nlohmann::json& participants);

// Safety-car transition → toast. status values are the session packet's
// safety_car_status (0=clear, 1=SC, 2=VSC, 3=formation). Returns nullopt when the
// transition shouldn't notify (e.g. first sight of "clear").
std::optional<ToastSpec> safetyCarToast(int oldStatus, int newStatus);
