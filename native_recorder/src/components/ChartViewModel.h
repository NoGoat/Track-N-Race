#pragma once

#include "ChartView.h"   // for ChartView::AxisSpec/SeriesSpec/Side

#include <QObject>
#include <QColor>
#include <QList>
#include <QPointF>
#include <QString>
#include <QVariantList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QQuickItem;
class QLineSeries;
class QAreaSeries;
class QValueAxis;
class QDateTimeAxis;
class QAbstractAxis;
class QAbstractSeries;
QT_END_NAMESPACE

// Backend engine + QML bridge for ChartView, on Qt Graphs (2D).
//
// Axes, ticks, grid and axis labels are now drawn NATIVELY by Qt Graphs: each
// axis is a QValueAxis / QDateTimeAxis with a printf labelFormat, so the bespoke
// overlay tick-labels/tick-marks are gone (and with them the things they enabled
// that the library can't do: sub-second time labels, colour-coded axis labels,
// background bands). The series are QLineSeries/QAreaSeries.
//
// Two overlays are KEPT custom, because Qt Graphs has no native equivalent:
//   * the hover crosshair + per-series value tooltip, and
//   * the legend.
// Both are bound from the QML overlay to the properties below.
//
// Decimation is also kept out of the library (Qt Graphs has none): the full raw
// samples live here and a reduced copy sized to the plot width is pushed to the
// line series on flush().
class ChartViewModel : public QObject {
    Q_OBJECT

    // Theme colours, kept in sync with the widget palette (see ChartView::changeEvent).
    Q_PROPERTY(QColor textColor     READ textColor     NOTIFY themeChanged)
    Q_PROPERTY(QColor axisLineColor READ axisLineColor NOTIFY themeChanged)
    Q_PROPERTY(QColor gridColor     READ gridColor     NOTIFY themeChanged)

    // Hover crosshair: position as a 0..1 fraction across the key (x) axis; QML
    // maps it onto plotArea. Hidden when crosshairVisible is false.
    Q_PROPERTY(bool   crosshairVisible READ crosshairVisible NOTIFY hoverChanged)
    Q_PROPERTY(qreal  crosshairT       READ crosshairT       NOTIFY hoverChanged)

    // Tooltip: rich-text body built in C++; QML renders and positions it.
    Q_PROPERTY(bool    tooltipVisible READ tooltipVisible NOTIFY hoverChanged)
    Q_PROPERTY(QString tooltipHtml    READ tooltipHtml    NOTIFY hoverChanged)

    Q_PROPERTY(bool legendVisible READ legendVisible NOTIFY legendChanged)
    Q_PROPERTY(QVariantList legendEntries READ legendEntries NOTIFY legendChanged)

public:
    explicit ChartViewModel(QObject* parent = nullptr);
    ~ChartViewModel() override;

    // Hook the QML GraphsView once ChartSurface.qml is Ready. Series/axes created
    // before this are buffered and flushed in on attach.
    void attach(QQuickItem* graphsView);

    // ── mirror of the ChartView public API ───────────────────────────────────
    int  addAxis(const ChartView::AxisSpec& spec);
    int  addSeries(const ChartView::SeriesSpec& spec);

    void appendPoint(int seriesId, double x, double y);
    void setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys);
    void trimBefore(int seriesId, double x);
    void clear(int seriesId);
    void clearAll();

    void setSeriesVisible(int seriesId, bool visible);
    void setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep);
    void setLegendVisible(bool on);
    void setHoverReadout(bool on);
    bool seriesKeyRange(int seriesId, double& lo, double& hi) const;
    void setXRange(int axisId, double min, double max);

    // Decimate + push every series to its QLineSeries.
    void flush(int plotWidthPx);

    void setThemeColors(const QColor& text, const QColor& line);

    // ── QML-facing reads/invokables ──────────────────────────────────────────
    QColor textColor()     const { return text_; }
    QColor axisLineColor() const;
    QColor gridColor()     const;
    bool   crosshairVisible() const { return hoverActive_; }
    qreal  crosshairT()       const { return crosshairT_; }
    bool   tooltipVisible()   const { return hoverActive_ && !tooltipHtml_.isEmpty(); }
    QString tooltipHtml()     const { return tooltipHtml_; }
    bool   legendVisible()    const { return legendVisible_; }
    // Overlay legend rows: { name, color } for named, visible series.
    QVariantList legendEntries() const;

    // QML reports the pointer as a 0..1 fraction across the plot's x extent.
    Q_INVOKABLE void hoverAt(qreal t);
    Q_INVOKABLE void hoverLeave();

signals:
    void themeChanged();
    void hoverChanged();
    void legendChanged();
    void replotRequested();

private:
    struct Axis {
        QValueAxis*    obj   = nullptr;   // value axis (null when dateTime)
        QDateTimeAxis* dtObj = nullptr;   // time axis (null unless dateTime)
        bool   dateTime = false;          // rendered natively as m:ss via dtObj
        ChartView::Side side = ChartView::Side::Left;
        bool   isX = false;
        double min = 0.0, max = 1.0;
        bool   grid = false;
        double scale = 1.0;     // Suffix label divisor
        QString suffix;         // Suffix label text
        double fixedStep = 0.0; // forced tick interval (raw value units); <=0 means auto
        int    precision = 0;
    };
    struct Series {
        QLineSeries* line = nullptr;       // the stroke (added to the view)
        QAreaSeries* area = nullptr;       // fill, drawn behind the line (added to the view)
        QLineSeries* areaUpper = nullptr;  // area's upperSeries — NOT added to the view
        QList<QPointF> raw;                // full, un-decimated samples (sorted by x)
        QString name;
        QColor  color;
        QString unit;
        int     precision = 0;
        bool    group = false;
        bool    visible = true;
        bool    fill = false;
        double  yScale = 1.0;   // y multiplier applied only when pushing to the plot
        int     xAxisId = -1, yAxisId = -1;
    };

    void addSeriesToView(QAbstractSeries* s);
    void applyAxisFormat(Axis& a);   // native labelFormat + tick interval
    void updateMargins();            // size the GraphsView plot margins for native labels
    QString labelFormatFor(const Axis& a) const;

    QQuickItem* view_ = nullptr;
    QList<Axis>   axes_;
    QList<Series> series_;

    int    keyAxisId_ = -1;     // first Bottom axis — the primary (GraphsView) x
    int    primaryLeftId_ = -1; // first Left axis — the primary (GraphsView) y
    QColor text_ = QColor(220, 220, 220);
    QColor line_ = QColor(150, 150, 150);   // grid/axis lines (QPalette::PlaceholderText)
    bool   legendVisible_ = true;
    bool   hoverEnabled_  = false;
    bool   hoverActive_   = false;
    qreal  crosshairT_    = 0.0;
    QString tooltipHtml_;
    int    lastPlotWidth_ = 1000;
    bool   gridXSet_ = false;   // GraphsView.axisX assigned (drives plot extent + grid)
    bool   gridYSet_ = false;   // GraphsView.axisY assigned
};
