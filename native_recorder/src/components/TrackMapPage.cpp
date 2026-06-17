#include "../MainWindow.h"
#include "TrackMapWidget.h"

#include <QApplication>
#include <QPalette>

// ── Track map updater ───────────────────────────────────────────────────────
// The map widget itself lives in the central area of the Session page
// (created in buildSessionPage); this only pushes live data into it.

void MainWindow::updateTrackMapPage() {
    if (!trackMap_) return;

    // Load the circuit geometry when the track changes.
    if (!lastSessionData.empty()) {
        int tid = lastSessionData.value("track_id", -1);
        if (tid >= 0 && tid != mapTrackId_) {
            trackMap_->setTrack(tid);
            mapTrackId_ = tid;
        }
    }

    // Theme: derive light/dark from the active palette.
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    trackMap_->setDark(dark);
    trackMap_->setLabelMode(static_cast<TrackMapWidget::LabelMode>(trackMapLabelMode()));

    if (!lastParticipantsData.empty())
        trackMap_->setParticipants(lastParticipantsData);
    if (!lastPositionsData.empty())
        trackMap_->setPositions(lastPositionsData);
}
