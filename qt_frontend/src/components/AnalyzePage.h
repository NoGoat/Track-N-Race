#pragma once

#include "AnalyzeMetrics.h"
#include <QSettings>
#include <QWidget>
#include <cstdint>

class AnalyzeChart;
class SessionModel;
class QCheckBox;
class QComboBox;
class QFrame;
class QListWidget;
class QPushButton;

class AnalyzePage : public QWidget {
    Q_OBJECT
public:
    explicit AnalyzePage(SessionModel* model, QWidget* parent = nullptr);
    void setPlaybackMode(bool on, float currentTime = 0);
    void setCurrentTime(float t);
    void resetPlaybackSelections();
    uint32_t playbackRowMask() const;
    QVector<int> requestedPlaybackLaps() const;

public slots:
    void zoomIn();
    void zoomOut();
    void panLeft();
    void panRight();
    void resetZoom();

signals:
    void navigationEnabledChanged(bool enabled);
    void dataRequirementsChanged();

private:
    SessionModel* model_ = nullptr;
    AnalyzeChart* chart_ = nullptr;
    QFrame* sidebar_ = nullptr;
    QComboBox* addMetric_ = nullptr;
    QCheckBox* fixedMode_ = nullptr;
    QCheckBox* showYAxis_ = nullptr;
    QComboBox* compareLap_ = nullptr;
    QComboBox* lapA_ = nullptr;
    QComboBox* lapB_ = nullptr;
    QListWidget* seriesList_ = nullptr;
    QPushButton* collapse_ = nullptr;
    QPushButton* expand_ = nullptr;
    QWidget* collapsedBar_ = nullptr;
    QPushButton* compareClear_ = nullptr;
    QVector<AnalyzeSeriesSetting> series_;
    QSettings settings_{"TrackNRace","NativeRecorder"};
    bool playback_ = false;
    bool collapsed_ = false;

    void loadSettings();
    void saveSettings();
    void rebuildMetricPicker();
    void rebuildSeriesList();
    void refreshLapSelectors();
    void applyState();
    void moveSeries(int from, int to);
};
