#pragma once

#include <QWidget>
#include <QPointer>

class ChartView;
class SessionModel;
class QTimer;

class TyreChartsWidget : public QWidget {
    Q_OBJECT
public:
    explicit TyreChartsWidget(QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);

public slots:
    void setCurrentTime(float t);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void requestRefresh();
    void refresh();

    QPointer<SessionModel> model_;
    QTimer*   refreshTimer_ = nullptr;
    bool      dirty_        = false;
    bool      playback_     = false;
    float     currentTime_  = 0.0f;
    float     windowS_      = 120.0f;   // 2 min default view window

    float currentTime() const;

    float prevEndTime_  = -9999.0f;
    float lastAddedTime_= -9999.0f;

    // Four chart panels: surface temp / inner temp / brake temp / tyre wear
    ChartView* surfChart_  = nullptr;
    ChartView* innerChart_ = nullptr;
    ChartView* brakeChart_ = nullptr;
    ChartView* wearChart_  = nullptr;

    // Series IDs for FL/FR/RL/RR per chart
    int surfIds_[4]  = {};
    int innerIds_[4] = {};
    int brakeIds_[4] = {};
    int wearIds_[4]  = {};

    // Shared time axis IDs per chart
    int surfXId_  = -1;
    int innerXId_ = -1;
    int brakeXId_ = -1;
    int wearXId_  = -1;
};
