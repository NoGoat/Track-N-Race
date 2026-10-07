#pragma once

#include "ChartView.h"
#include "../TrendMeasures.h"

#include <QString>

class QLabel;
struct SessionData;

// The Trends page's tyre graph (Electron StintTyreWearChart): the Tyres page
// wear chart — four corners of wear or life over session time — scoped to the
// current stint or all laps by its own header control rather than the chart
// window, with lap numbers at the lap lines. It joins the page's shared
// tooltip after the per-lap graphs.
class TrendTyreChart : public ChartView {
    Q_OBJECT

public:
    explicit TrendTyreChart(QWidget* parent = nullptr);

    void setLifeMode(bool life);
    void refreshColors();
    // Draws the damage history from `lower` (the stint start, or the first
    // lap) to the cursor. `revision` changes whenever held rows were rewritten.
    void refresh(const SessionData& data, const QVector<TrendLapBoundary>& boundaries,
                 float endTime, double lower, bool dynamicY, quint64 revision);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    int xId_ = -1;
    int yId_ = -1;
    int ids_[4] = {-1, -1, -1, -1};
    QLabel* empty_ = nullptr;   // Electron's "No data" while there is no line to draw
    bool life_ = true;
    QString dataKey_;
    float lastAddedTime_ = -1.0f;
    float prevEndTime_ = -1.0f;
};
