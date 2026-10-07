#pragma once

#include "ChartView.h"

#include <QColor>
#include <QString>
#include <QVector>

#include <functional>
#include <limits>

// One point of a Trends graph (Electron TrendChartPoint). NaN values are gaps.
struct TrendPoint {
    double x = 0;
    QVector<double> values;
    QString label;
    bool invalid = false;
    bool fastest = false;
    bool provisional = false;   // the lap in progress: its value is still rising
    // Session time the lap began, which places it on session-time charts.
    double lapStart = std::numeric_limits<double>::quiet_NaN();
    // The lap a sample was taken in, when `x` falls inside it (0 = none).
    int lap = 0;
};

// Listed top to bottom: the first series is drawn above the rest, and the
// legend and tooltip follow the same order. `axis` indexes the chart's axes.
// `perLap` series read the lap points, the rest the points; each reads value
// `channel`, by default its position within its own group. In bars, series
// sharing a `stack` share one bar, stacked bottom up in the order listed.
struct TrendSeries {
    QString label;
    QColor color;
    int axis = 0;
    bool perLap = false;
    int channel = -1;
    QString stack;
};

using TrendFormat = std::function<QString(double)>;

// An independent value scale: the first on the left, the rest on the right.
struct TrendAxis {
    double min = 0;
    double max = 1;
    TrendFormat format;
    TrendFormat formatTooltip;   // falls back to `format`
    QColor color;                // label colour; the neutral axis colour when invalid
};

struct TrendRange {
    double min = 0;
    double max = 1;
};

// The narrowest range covering [lo, hi] whose quarter lines are round values.
// A finite `floor` pins the bottom of the range, such as 0 MJ or 0% wear.
TrendRange quarterAxisRange(double lo, double hi,
                            double floor = std::numeric_limits<double>::quiet_NaN());

// Electron's TrendChart: one Trends graph drawn per lap on a lap-number axis,
// with its title, a header control and a clickable legend. Three forms:
//   lines   — `points` joined per series;
//   combined — `points` (tyre samples) plus `lapPoints` drawn above them,
//              joined directly with a dot per lap, each series on its axis;
//   bars    — one filled bar per slot around each lap, stacks bottom up.
// With `axes`, every axis labels its own quarter lines; without, one left
// axis fits the points (or starts at zero with `zeroBaselineHeadroom`).
class TrendChart : public ChartView {
    Q_OBJECT

public:
    struct Config {
        QString title;
        QVector<TrendSeries> series;
        int axisCount = 0;   // > 0: the chart takes that many TrendAxis scales
        bool bars = false;
        bool discrete = false;
        double zeroBaselineHeadroom = std::numeric_limits<double>::quiet_NaN();
        TrendFormat formatY;
        TrendFormat formatTooltipY;   // falls back to formatY
        // >= 0: joins the page's shared tooltip at this order, matched to
        // time charts by lap start.
        int syncOrder = -1;
    };

    explicit TrendChart(const Config& config, QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setSeriesColors(const QVector<QColor>& colors);
    // `axes` must hold Config::axisCount scales when the chart has them.
    void setData(const QVector<TrendPoint>& points, const QVector<TrendPoint>& lapPoints = {},
                 const QVector<TrendAxis>& axes = {});
    void setHeaderControl(QWidget* control) { setPanelHeaderControl(0, control); }
    // Cursor sync for a chart with a sync order; the others never sync.
    void applyCursorSync(bool enabled, bool secondaryVertical, bool secondaryHorizontal);

protected:
    bool customTooltip(int panelId, double key, TooltipContent& out) const override;
    bool customSyncedRows(int panelId, double key, TooltipContent& out) const override;
    bool hoverSample(int panelId, double key, double& sampled) const override;

private:
    int channelOf(int index) const;
    // The x of the row a hover at `key` snaps to (bar corners in bars).
    double snappedRowX(double key) const;
    int nearestPoint(double x) const;
    QString pointLabel(const TrendPoint& point) const;
    TrendFormat tooltipFormatFor(int seriesIndex) const;
    TooltipContent valueLines(const TrendPoint* main, const TrendPoint* lap) const;
    void updateYAxes();

    Config config_;
    QVector<int> lineIds_;   // per config series: the named (legend) series
    QVector<int> dotIds_;    // per config series: the per-lap dots, or -1
    QVector<int> yIds_;      // one per scale (one when the chart has none)
    int xId_ = -1;
    QVector<TrendPoint> points_;
    QVector<TrendPoint> lapPoints_;
    QVector<TrendAxis> axes_;
    QVector<double> rowXs_;  // the rows a hover snaps to, ascending
};
