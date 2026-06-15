#pragma once

#include <QWidget>
#include <QColor>
#include <QString>
#include <QVector>
#include <memory>

// Generic, backend-agnostic time-series chart widget.
//
// Wraps a charting backend (currently QCustomPlot) but exposes none of its
// types — the backend lives entirely behind a pimpl in ChartView.cpp, so it can
// be swapped without touching any caller. Charts are declared by config:
// add some axes, add some series bound to those axes, then push data. Series use
// adaptive sampling (per-pixel decimation) so paint cost stays flat even with
// tens of thousands of retained points.
class ChartView : public QWidget {
    Q_OBJECT

public:
    enum class Side { Bottom, Left, Right };

    struct AxisSpec {
        Side    side         = Side::Left;
        double  min          = 0.0;
        double  max          = 1.0;
        QColor  labelColor   = QColor();   // invalid → inherit theme text color
        bool    visible      = true;
        char    numberFormat = 'f';        // QCPAxis number format: 'f', 'g', 'e'
        int     precision    = 0;          // tick label decimal precision
    };

    struct SeriesSpec {
        QString name;
        QColor  color = QColor("#888888");
        double  width = 2.0;
        int     xAxisId = -1;            // axis id returned by addAxis()
        int     yAxisId = -1;
    };

    explicit ChartView(QWidget* parent = nullptr);
    ~ChartView() override;

    // Declare the chart. addAxis/addSeries return opaque ids used by the data
    // calls below. Series are drawn in creation order (later draws on top).
    int  addAxis(const AxisSpec& spec);
    int  addSeries(const SeriesSpec& spec);

    // Data.
    void appendPoint(int seriesId, double x, double y);
    void setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys);
    void trimBefore(int seriesId, double x);   // drop samples with key < x
    void clear(int seriesId);
    void clearAll();

    // Hide a series (and its legend entry) — used to switch between single-lap
    // and two-lap overlay layouts without rebuilding the chart.
    void setSeriesVisible(int seriesId, bool visible);

    // Format an axis's tick labels as time (QCPAxisTickerTime, e.g. "%m:%s").
    void setAxisTimeTicker(int axisId, const QString& format);

    // Enable a crosshair + value readout that tracks the cursor across the chart,
    // showing the nearest value of each visible series (and the x as m:ss.s).
    void setHoverReadout(bool on);

    // Current min/max x (key) of a series. Returns false if the series is empty.
    bool seriesKeyRange(int seriesId, double& lo, double& hi) const;

    // View. requestReplot() coalesces multiple updates in a frame into one paint.
    void setXRange(int axisId, double min, double max);
    void requestReplot();

protected:
    void changeEvent(QEvent* e) override;   // keep label/legend colors in sync with the theme

private:
    void applyPaletteText();

    struct Impl;
    std::unique_ptr<Impl> d_;
};
