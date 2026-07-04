#pragma once

#include <QWidget>
#include <QSettings>

#include "MiscLayout.h"

class QFrame;
class GForceChart;
class RideHeightChart;
class SessionModel;

// Misc tab — G-force and ride-height charts bound to the SessionModel.
// Self-contained: owns its widgets and layout persistence; MainWindow only
// forwards playback state through the setters below.
class MiscPage : public QWidget {
    Q_OBJECT

public:
    explicit MiscPage(SessionModel* model, QWidget* parent = nullptr);

    // "Edit Layout" dialog reads/writes through these (immediate-apply).
    MiscLayout loadLayout();
    void applyAndSaveLayout(const MiscLayout& layout);

    // Playback plumbing — forwarded to the charts.
    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);

private:
    void saveLayout(const MiscLayout& layout);
    void applyLayout(const MiscLayout& layout);

    QWidget*         gforceContainer_     = nullptr;
    QWidget*         rideHeightContainer_ = nullptr;
    QFrame*          hdiv_                = nullptr;
    GForceChart*     gforceChart_         = nullptr;
    RideHeightChart* rideHeightChart_     = nullptr;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
