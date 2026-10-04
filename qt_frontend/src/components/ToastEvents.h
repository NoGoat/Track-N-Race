#pragma once

#include <optional>
#include <QString>
#include <QColor>

#include <tnrp/control_rows.h>

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
// buildBanner() (electron-frontend/src/renderer/src/App.tsx:161).

// race_event row → toast, or nullopt for codes that shouldn't notify (a SCAR only
// toasts its transient green "Resume Race"; OVTK/SPTP are intentionally silent).
// `participants` is the latest "participants" row for name lookup (nullptr =
// none seen yet).
std::optional<ToastSpec> buildToast(const tnrp::RaceEventRow& event,
                                    const tnrp::ParticipantsRow* participants);

// Electron's safety-car labels (bannerHelpers.ts), shared with the events list.
// type: 1 = SC, 2 = VSC, 3 = formation lap. Action: 0 Deployed, 1 Returning
// ("Ending" for SC/VSC), 2 Returned, 3 Resume Race; SC/VSC Deployed has none.
QString safetyCarTypeLabel(int type);
QString safetyCarActionLabel(int type, int action);   // empty = no sub-line

// The persistent safety-car toast, derived like Electron's useRaceBanners():
// from the session's safety_car_status (nullopt = no session row) and the most
// recent SCAR event (nullptr = none). nullopt means no banner should be shown.
std::optional<ToastSpec> safetyCarBanner(std::optional<int> safetyCarStatus,
                                         const tnrp::RaceEventRow* latestScar);

ToastSpec raceLeaderToast(int carIdx, const tnrp::ParticipantsRow* participants);
