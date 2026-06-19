#pragma once

#include "ChartView.h"   // for ChartView::AxisSpec/SeriesSpec/BandSpec/Side

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
class QAbstractSeries;
QT_END_NAMESPACE

// Backend engine + QML bridge for ChartView, replacing the old QCustomPlot Impl.
//
// Owns the Qt Graphs objects (QLineSeries/QAreaSeries/QValueAxis) and the full
// raw sample store. The GraphsView lives in ChartSurface.qml; once that scene is
// ready ChartView calls attach() with the GraphsView item, and every series/axis
// created afterwards is wired straight into it. On flush() the raw data is
// decimated to the plot's pixel width and pushed to the line series — the
// stand-in for QCustomPlot's adaptive sampling.
//
// QML binds to the exposed properties/signals to draw what Qt Graphs can't: the
// custom axis tick labels (time "m:ss.t", scaled "16k", "%"), the hover crosshair
// and the per-series tooltip, the overlay legend, and theme-driven colors.
class ChartViewModel : public QObject {
    Q_OBJECT

    // Theme colors, kept in sync with the widget palette (see ChartView::changeEvent).
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

    // Lists the overlays bind to; each re-evaluates when its NOTIFY fires.
    Q_PROPERTY(QVariantList axisLabels    READ axisLabels    NOTIFY labelsChanged)
    Q_PROPERTY(QVariantList legendEntries READ legendEntries NOTIFY legendChanged)
    Q_PROPERTY(QVariantList bandRects     READ bandRects     NOTIFY bandsChanged)

public:
    explicit ChartViewModel(QObject* parent = nullptr);
    ~ChartViewModel() override;

    // Hook the QML GraphsView once ChartSurface.qml is Ready. Series/axes created
    // before this are buffered and flushed in on attach.
    void attach(QQuickItem* graphsView);

    // ── mirror of the ChartView public API ───────────────────────────────────
    int  addAxis(const ChartView::AxisSpec& spec);
    int  addSeries(const ChartView::SeriesSpec& spec);
    void addBand(const ChartView::BandSpec& spec);

    void appendPoint(int seriesId, double x, double y);
    void setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys);
    void trimBefore(int seriesId, double x);
    void clear(int seriesId);
    void clearAll();

    void setSeriesVisible(int seriesId, bool visible);
    void setAxisTimeTicker(int axisId);
    void setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep);
    void setLegendVisible(bool on);
    void setHoverReadout(bool on);
    bool seriesKeyRange(int seriesId, double& lo, double& hi) const;
    void setXRange(int axisId, double min, double max);

    // Decimate + push every series to its QLineSeries and recompute tick labels.
    void flush(int plotWidthPx);

    void setThemeColors(const QColor& text);

    // ── QML-facing reads/invokables ──────────────────────────────────────────
    QColor textColor()     const { return text_; }
    QColor axisLineColor() const;
    QColor gridColor()     const;
    bool   crosshairVisible() const { return hoverActive_; }
    qreal  crosshairT()       const { return crosshairT_; }
    bool   tooltipVisible()   const { return hoverActive_ && !tooltipHtml_.isEmpty(); }
    QString tooltipHtml()     const { return tooltipHtml_; }
    bool   legendVisible()    const { return legendVisible_; }

    // Flat list of every axis tick to draw: { isX, alignment, t, text, color }.
    // alignment uses Qt::Alignment ints (AlignBottom/AlignLeft/AlignRight).
    QVariantList axisLabels() const { return labels_; }
    // Overlay legend rows: { name, color } for named, visible series.
    QVariantList legendEntries() const;
    // Background bands as { t0, t1, color } fractions up the band's y axis; QML
    // maps them onto plotArea and draws them behind the series (GearChart).
    QVariantList bandRects() const;

    // QML reports the pointer as a 0..1 fraction across the plot's x extent.
    Q_INVOKABLE void hoverAt(qreal t);
    Q_INVOKABLE void hoverLeave();

signals:
    void themeChanged();
    void hoverChanged();
    void legendChanged();
    void labelsChanged();
    void bandsChanged();
    void replotRequested();   // QML may use this to nudge the overlay repaint

private:
    enum class Formatter { Plain, Time, Suffix };

    struct Axis {
        QValueAxis*  obj = nullptr;
        ChartView::Side side = ChartView::Side::Left;
        bool   isX = false;
        int    sideIndex = 0;   // order among axes on the same side (label column)
        double min = 0.0, max = 1.0;
        bool   inheritColor = true;
        QColor color;
        bool   grid = false;
        Formatter fmt = Formatter::Plain;
        double scale = 1.0;     // Suffix
        QString suffix;         // Suffix
        double fixedStep = 0.0; // Suffix; <=0 means auto
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
        int     xAxisId = -1, yAxisId = -1;
    };
    struct Band { int axisId = -1; double min = 0.0, max = 0.0; QColor color; };

    void buildLabels();
    void appendTicksFor(const Axis& a);
    double stepFor(const Axis& a) const;
    QString formatTick(const Axis& a, double value) const;
    void addSeriesToView(QAbstractSeries* s);

    QQuickItem* view_ = nullptr;
    QList<Axis>   axes_;
    QList<Series> series_;
    QList<Band>   bands_;

    int    keyAxisId_ = -1;     // first Bottom axis — the primary (GraphsView) x
    int    primaryLeftId_ = -1; // first Left axis — the primary (GraphsView) y
    QColor text_ = QColor(220, 220, 220);
    bool   legendVisible_ = true;
    bool   hoverEnabled_  = false;
    bool   hoverActive_   = false;
    qreal  crosshairT_    = 0.0;
    QString tooltipHtml_;
    int    lastPlotWidth_ = 1000;
    QVariantList labels_;
    bool   gridXSet_ = false;   // GraphsView.axisX assigned (drives plot extent + grid)
    bool   gridYSet_ = false;   // GraphsView.axisY assigned
};
