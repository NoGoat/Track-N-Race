#pragma once

#include "ChartView.h"
#include "AnalyzeMetrics.h"

#include <QPointer>
#include <QString>
#include <QVector>

class SessionModel;
struct LapBlock;
struct SessionData;

class AnalyzeChart : public ChartView {
    Q_OBJECT
public:
    explicit AnalyzeChart(QWidget* parent = nullptr);
    void setModel(SessionModel* model);
    void setConfig(const QVector<AnalyzeSeriesSetting>& series, bool showYAxis);
    void setPlaybackMode(bool on);
    void setCurrentTime(float t);
    void setDistanceMode(bool on);
    void setIndividualGraphs(bool on, bool syncedTooltip);
    void setSectorOptions(bool boundaries, bool sectorDelta);
    void setSecondarySource(const SessionData* catalog, const QHash<int,LapBlock>* cache);
    void setComparisonLap(int lapNum, bool secondary = false);
    void setFixedLaps(bool enabled, int lapA, bool lapASecondary,
                      int lapB, bool lapBSecondary);

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
    struct StackedHandles { int panel=-1,xAxis=-1,yAxis=-1,current=-1,comparison=-1; };
    QPointer<SessionModel> model_;
    QVector<AnalyzeSeriesSetting> selected_;
    QVector<Handles> handles_;
    QVector<StackedHandles> stacked_;
    QHash<QString,int> axes_;
    int xAxis_ = -1;
    int deltaAxis_ = -1;
    Handles deltaHandles_;
    StackedHandles stackedDelta_;
    bool showYAxis_ = true;
    bool playback_ = false;
    bool distanceMode_ = false;
    bool individualGraphs_ = false;
    bool syncedTooltip_ = false;
    bool sectorBoundaries_ = false;
    bool sectorDelta_ = false;
    const SessionData* secondaryData_ = nullptr;
    const QHash<int,LapBlock>* secondaryCache_ = nullptr;
    float currentTime_ = 0;
    int compareLap_ = -1;
    bool compareSecondary_ = false;
    bool fixed_ = false;
    int lapA_ = -1, lapB_ = -1;
    bool lapASecondary_ = false, lapBSecondary_ = false;
    bool dirty_ = true;
    QString fixedDomainKey_;
    QString panelLayoutKey_;

    void requestRefresh();
    void refresh();
};
