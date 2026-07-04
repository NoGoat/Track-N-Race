#pragma once

#include <QWidget>
#include <QSettings>

#include "InputLayout.h"

class QFrame;
class GearChart;
class InputsChart;
class SteeringChart;
class SessionModel;

// Input tab — gear / throttle-brake / steering charts bound to the
// SessionModel. Self-contained: owns its widgets and layout persistence;
// MainWindow only forwards playback state through the setters below.
class InputPage : public QWidget {
    Q_OBJECT

public:
    explicit InputPage(SessionModel* model, QWidget* parent = nullptr);

    // "Edit Layout" dialog reads/writes through these (immediate-apply).
    InputLayout loadLayout();
    void applyAndSaveLayout(const InputLayout& layout);

    // Playback plumbing — forwarded to the charts.
    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);

private:
    void saveLayout(const InputLayout& layout);
    void applyLayout(const InputLayout& layout);

    QWidget*       topRow_            = nullptr;
    QWidget*       gearContainer_     = nullptr;
    QWidget*       inputsContainer_   = nullptr;
    QFrame*        vdiv_              = nullptr;
    QWidget*       steeringContainer_ = nullptr;
    QFrame*        hdiv_              = nullptr;
    GearChart*     gearChart_         = nullptr;
    InputsChart*   inputsChart_       = nullptr;
    SteeringChart* steeringChart_     = nullptr;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
