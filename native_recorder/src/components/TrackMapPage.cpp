#include "../MainWindow.h"
#include "TrackMapWidget.h"

#include <QVBoxLayout>
#include <QApplication>
#include <QPalette>

// ── Track map page ──────────────────────────────────────────────────────────

QWidget* MainWindow::buildTrackMapPage() {
    QWidget* page = new QWidget;
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    trackMap_ = new TrackMapWidget;
    root->addWidget(trackMap_, 1);
    return page;
}

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

    if (!lastParticipantsData.empty())
        trackMap_->setParticipants(lastParticipantsData);
    if (!lastPositionsData.empty())
        trackMap_->setPositions(lastPositionsData);
}
