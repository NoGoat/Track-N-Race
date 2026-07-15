#pragma once

#include "ChartView.h"
#include "AnalyzeMetrics.h"

#include <QPointer>
#include <QString>
#include <QVector>

class SessionModel;
class QTimer;

class AnalyzeChart : public ChartView {
    Q_OBJECT
public:
    explicit AnalyzeChart(QWidget* parent = nullptr);
    void setModel(SessionModel* model);
    void setConfig(const QVector<AnalyzeSeriesSetting>& series, bool showYAxis);
    void setPlaybackMode(bool on);
    void setCurrentTime(float t);
    void setComparisonLap(int lapNum);
    void setFixedLaps(bool enabled, int lapA, int lapB);

public slots:
    void zoomIn() { zoomX(0.7); }
    void zoomOut() { zoomX(1.0/0.7); }
    void panLeft() { panX(-0.2); }
    void panRight() { panX(0.2); }
    void resetZoom() { resetX(); }

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct Handles { int current=-1, comparison=-1; };
    QPointer<SessionModel> model_;
    QTimer* refreshTimer_ = nullptr;
    QVector<AnalyzeSeriesSetting> selected_;
    QVector<Handles> handles_;
    QHash<QString,int> axes_;
    int xAxis_ = -1;
    bool showYAxis_ = true;
    bool playback_ = false;
    float currentTime_ = 0;
    int compareLap_ = -1;
    bool fixed_ = false;
    int lapA_ = -1, lapB_ = -1;
    bool dirty_ = true;
    QString fixedDomainKey_;

    void requestRefresh();
    void refresh();
};
