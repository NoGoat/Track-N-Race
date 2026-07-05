#pragma once

#include <QWidget>
#include <QSettings>

#include "InputLayout.h"

class InputChartsWidget;
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

    InputChartsWidget* charts_ = nullptr;   // gear / throttle-brake / steering, one plot

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
