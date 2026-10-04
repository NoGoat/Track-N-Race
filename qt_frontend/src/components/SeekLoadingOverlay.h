#pragma once

#include <QWidget>

class QTimer;
class QVariantAnimation;

// Electron's SeekLoadingOverlay: armed for the whole pending seek but kept
// hidden for the first 300 ms, so seeks that install quickly are never seen.
// Once shown it fades in over a near-black scrim with "LOADING" and an
// indeterminate bar, and swallows mouse input so the stale pages underneath
// cannot be hovered or clicked. It covers only its parent (the page stack),
// so the playback bar below stays usable.
class SeekLoadingOverlay : public QWidget {
    Q_OBJECT

public:
    explicit SeekLoadingOverlay(QWidget* parent);

    void arm();      // seek started: show after the delay unless disarmed first
    void disarm();   // seek installed, gate reset or playback exited

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void reveal();

    QTimer* delay_ = nullptr;
    QVariantAnimation* fade_ = nullptr;
    QVariantAnimation* scan_ = nullptr;
    double opacity_ = 0;
    double scanPos_ = 0;   // bar's left edge as a fraction of the track, -0.5 → 1
};
