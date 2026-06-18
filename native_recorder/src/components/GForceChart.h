#pragma once

#include "ChartView.h"

#include <QPointer>
#include <QVector>

class SessionModel;
class QTimer;

class GForceChart : public ChartView {
    Q_OBJECT

public:
    explicit GForceChart(QWidget* parent = nullptr);

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

    void buildDefault(float endTime);

    float currentTime() const;

    QPointer<SessionModel> model_;
    QTimer*   refreshTimer_ = nullptr;
    bool      dirty_        = false;

    bool      playback_     = false;
    float     currentTime_  = 0.0f;
    float     windowS_      = 30.0f;
    float     prevEndTime_  = -1.0f;
    float     lastAddedTime_= -1.0f;

    int axXId_ = -1;
    int latId_ = -1;
    int longId_ = -1;
};
