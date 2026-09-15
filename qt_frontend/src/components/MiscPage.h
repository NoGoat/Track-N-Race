#pragma once

#include <QWidget>
#include <QSettings>

#include "MiscLayout.h"
#include "../GraphViewSettings.h"
#include <QVector>

class MiscChartsWidget;
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
    bool splitLayout(bool gForce) const;
    void setSplitLayout(bool gForce, bool split);
    QVector<tnr::GraphSection> chartSections();

    // Playback plumbing — forwarded to the charts.
    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);

    // Combined G/ride=0/1, lateral/longitudinal/front/rear=2..5.
    void setGraphSectionTable(int section, bool table);

signals:
    void layoutChanged();

private:
    void saveLayout(const MiscLayout& layout);
    void applyLayout(const MiscLayout& layout);

    MiscChartsWidget* charts_ = nullptr;   // G-force / ride-height, one plot

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
