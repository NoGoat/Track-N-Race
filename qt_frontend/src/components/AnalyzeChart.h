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
    void setLabels(const QString& current, const QString& comparison);
    void setMapCursors(bool visible, const QColor& currentColor,
                       const QColor& comparisonColor);
    void setMapCursorElapsed(double elapsedSeconds);
    void setSelectedLaps(bool fixed,
                         const SessionData* primaryData, const LapBlock* primary,
                         const SessionData* comparisonData, const LapBlock* comparison);

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
    const SessionData* selectedPrimaryData_ = nullptr;
    const SessionData* selectedComparisonData_ = nullptr;
    const LapBlock* selectedPrimary_ = nullptr;
    const LapBlock* selectedComparison_ = nullptr;
    float currentTime_ = 0;
    bool fixed_ = false;
    bool dirty_ = true;
    QString fixedDomainKey_;
    QString panelLayoutKey_;
    QString currentLabel_{"Current"};
    QString comparisonLabel_{"Compare"};
    bool mapCursorsVisible_ = false;
    double mapCursorElapsed_ = 0.0;
    QColor mapCurrentColor_{"#5794F2"};
    QColor mapComparisonColor_{"#C4162A"};

    void requestRefresh();
    void refresh();
    void refreshMapCursorGuides();
};
