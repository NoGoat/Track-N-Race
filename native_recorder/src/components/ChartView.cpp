#include "ChartView.h"

#include "../third_party/qcustomplot/qcustomplot.h"

#include <QEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <cmath>

namespace {
// Subtle grey shared by every axis line, tick and grid, so the dark chart reads
// like the Electron version rather than drawing axes in (invisible) black.
const QColor AXIS_LINE(150, 150, 150, 130);
const QColor GRID_LINE(150, 150, 150, 40);

// Per-pixel decimation (adaptive sampling) is only enabled once the visible time
// window reaches this width. Below it the on-screen sample count is small enough
// to draw raw — crisper lines and exact hover values, no measurable paint cost.
const double DECIMATE_MIN_WINDOW_S = 120.0;   // 2 minutes

// Ticker that scales the value and appends a suffix: "16000" -> "16k", "80" -> "80%".
// A positive fixedStep forces evenly spaced ticks (e.g. every 2000 for RPM).
class SuffixTicker : public QCPAxisTicker {
public:
    SuffixTicker(double scale, QString suffix, double fixedStep = 0.0)
        : scale_(scale), suffix_(std::move(suffix)), step_(fixedStep) {}
protected:
    double getTickStep(const QCPRange& range) override {
        return step_ > 0 ? step_ : QCPAxisTicker::getTickStep(range);
    }
    QString getTickLabel(double tick, const QLocale& locale, QChar formatChar, int precision) override {
        return QCPAxisTicker::getTickLabel(tick / scale_, locale, formatChar, precision) + suffix_;
    }
private:
    double  scale_;
    QString suffix_;
    double  step_;
};

// Time ticker rendering m:ss.t (tenths), e.g. "0:10.0", like the Electron chart.
class TenthsTimeTicker : public QCPAxisTickerTime {
protected:
    QString getTickLabel(double tick, const QLocale&, QChar, int) override {
        const int tenths = int(std::llround(tick * 10.0));
        const int whole  = tenths / 10;
        return QString("%1:%2.%3")
            .arg(whole / 60).arg(whole % 60, 2, 10, QChar('0')).arg(tenths % 10);
    }
};
}

// All QCustomPlot specifics live here, behind ChartView's backend-agnostic API.
struct ChartView::Impl {
    // Per-series tooltip metadata, parallel to graphs.
    struct SeriesMeta { QString unit; int precision = 0; bool group = false; };

    QCustomPlot*         plot = nullptr;
    QVector<QCPAxis*>    axes;            // by axis id
    QVector<bool>        axisInheritColor;
    QVector<QCPGraph*>   graphs;          // by series id
    QVector<SeriesMeta>  meta;            // by series id

    QCPAxis*      keyAxis   = nullptr;    // first Bottom axis — the shared x
    bool          hoverOn   = false;
    QCPItemLine*  crosshair = nullptr;
    QLabel*       tooltip   = nullptr;    // floating, cursor-following value readout

    static QCPAxis::AxisType toQcp(Side s) {
        switch (s) {
            case Side::Bottom: return QCPAxis::atBottom;
            case Side::Left:   return QCPAxis::atLeft;
            case Side::Right:  return QCPAxis::atRight;
        }
        return QCPAxis::atLeft;
    }
};

ChartView::ChartView(QWidget* parent)
    : QWidget(parent), d_(std::make_unique<Impl>())
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(120);

    d_->plot = new QCustomPlot(this);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(d_->plot);

    QCustomPlot* p = d_->plot;

#ifdef QCUSTOMPLOT_USE_OPENGL
    // GPU rasterization. The software QPainter path costs ~90 ms/frame even with
    // little data here, and seconds/frame at a 10-min window; OpenGL renders the
    // same content in single-digit ms (measured). 4x multisampling keeps lines
    // smooth without the cost/compat risk of the 16x default.
    p->setOpenGl(true, 16);
#endif
    p->setBackground(Qt::NoBrush);
    p->setBackground(QBrush(Qt::transparent));
    // Auto margins so the value/time tick labels have room to draw (with zero
    // margins QCustomPlot renders them off-widget and they vanish).
    p->axisRect()->setAutoMargins(QCP::msAll);

    // We declare every axis explicitly via addAxis(); hide the four defaults so
    // they don't draw duplicate frames/grids.
    for (QCPAxis* def : { p->xAxis, p->yAxis, p->xAxis2, p->yAxis2 })
        def->setVisible(false);

    // Realtime overlay: a transparent, top-centered legend with no border.
    p->legend->setVisible(true);
    p->legend->setBrush(QBrush(Qt::transparent));
    p->legend->setBorderPen(Qt::NoPen);
    p->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignTop | Qt::AlignHCenter);

    applyPaletteText();
}

ChartView::~ChartView() = default;

int ChartView::addAxis(const AxisSpec& spec)
{
    QCPAxis* ax = d_->plot->axisRect()->addAxis(Impl::toQcp(spec.side));
    ax->setRange(spec.min, spec.max);
    ax->setVisible(spec.visible);
    ax->setNumberFormat(QString(QChar(spec.numberFormat)));
    ax->setNumberPrecision(spec.precision);

    // Grey axis line + ticks (the default is black, invisible on the dark chart).
    ax->setBasePen(QPen(AXIS_LINE));
    ax->setTickPen(QPen(AXIS_LINE));
    ax->setSubTickPen(QPen(AXIS_LINE));
    ax->grid()->setVisible(spec.grid);
    ax->grid()->setPen(QPen(GRID_LINE));
    ax->grid()->setSubGridVisible(false);

    const bool inherit = !spec.labelColor.isValid();
    if (!inherit) {
        ax->setTickLabelColor(spec.labelColor);
        ax->setLabelColor(spec.labelColor);
    }

    const int id = d_->axes.size();
    d_->axes.append(ax);
    d_->axisInheritColor.append(inherit);
    if (spec.side == Side::Bottom && !d_->keyAxis) d_->keyAxis = ax;
    applyPaletteText();
    return id;
}

int ChartView::addSeries(const SeriesSpec& spec)
{
    QCPAxis* kax = (spec.xAxisId >= 0 && spec.xAxisId < d_->axes.size())
                       ? d_->axes[spec.xAxisId] : d_->plot->xAxis;
    QCPAxis* vax = (spec.yAxisId >= 0 && spec.yAxisId < d_->axes.size())
                       ? d_->axes[spec.yAxisId] : d_->plot->yAxis;

    QCPGraph* g = d_->plot->addGraph(kax, vax);
    g->setName(spec.name);
    g->setPen(QPen(spec.color, spec.width));
    // Decimation is gated on the current window width (see setXRange); a new
    // series inherits whatever the shared key axis already implies.
    const double window = d_->keyAxis ? d_->keyAxis->range().size() : 0.0;
    g->setAdaptiveSampling(window >= DECIMATE_MIN_WINDOW_S);
    if (spec.step) g->setLineStyle(QCPGraph::lsStepLeft);

    if (spec.fill) {
        QColor c = spec.fillColor.isValid() ? spec.fillColor : spec.color;
        c.setAlpha(40);
        g->setBrush(QBrush(c));
    }

    const int id = d_->graphs.size();
    d_->graphs.append(g);
    d_->meta.append({ spec.unit, spec.tipPrecision, spec.tipGroupThousands });
    applyPaletteText();
    return id;
}

void ChartView::addBand(const BandSpec& spec)
{
    QCPAxis* vax = (spec.axisId >= 0 && spec.axisId < d_->axes.size())
                       ? d_->axes[spec.axisId] : d_->plot->yAxis;

    QCPItemRect* rect = new QCPItemRect(d_->plot);
    rect->setClipToAxisRect(true);

    rect->topLeft->setTypeX(QCPItemPosition::ptAxisRectRatio);
    rect->topLeft->setTypeY(QCPItemPosition::ptPlotCoords);
    rect->topLeft->setAxes(d_->keyAxis, vax);
    rect->topLeft->setCoords(0.0, spec.max);

    rect->bottomRight->setTypeX(QCPItemPosition::ptAxisRectRatio);
    rect->bottomRight->setTypeY(QCPItemPosition::ptPlotCoords);
    rect->bottomRight->setAxes(d_->keyAxis, vax);
    rect->bottomRight->setCoords(1.0, spec.min);

    rect->setPen(Qt::NoPen);
    rect->setBrush(QBrush(spec.color));

    rect->setLayer("background");
}

void ChartView::appendPoint(int seriesId, double x, double y)
{
    if (seriesId < 0 || seriesId >= d_->graphs.size()) return;
    d_->graphs[seriesId]->addData(x, y);
}

void ChartView::setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys)
{
    if (seriesId < 0 || seriesId >= d_->graphs.size()) return;
    d_->graphs[seriesId]->setData(xs, ys, /*alreadySorted=*/true);
}

void ChartView::trimBefore(int seriesId, double x)
{
    if (seriesId < 0 || seriesId >= d_->graphs.size()) return;
    d_->graphs[seriesId]->data()->removeBefore(x);
}

void ChartView::clear(int seriesId)
{
    if (seriesId < 0 || seriesId >= d_->graphs.size()) return;
    d_->graphs[seriesId]->data()->clear();
}

void ChartView::clearAll()
{
    for (QCPGraph* g : d_->graphs) g->data()->clear();
}

void ChartView::setSeriesVisible(int seriesId, bool visible)
{
    if (seriesId < 0 || seriesId >= d_->graphs.size()) return;
    QCPGraph* g = d_->graphs[seriesId];
    g->setVisible(visible);
    // Unnamed series (e.g. muted reference traces) never clutter the legend.
    if (visible && !g->name().isEmpty()) g->addToLegend();
    else                                 g->removeFromLegend();
}

void ChartView::setLegendVisible(bool on)
{
    d_->plot->legend->setVisible(on);
}

void ChartView::setAxisTimeTicker(int axisId, const QString& /*format*/)
{
    if (axisId < 0 || axisId >= d_->axes.size()) return;
    d_->axes[axisId]->setTicker(QSharedPointer<TenthsTimeTicker>::create());
}

void ChartView::setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep)
{
    if (axisId < 0 || axisId >= d_->axes.size()) return;
    d_->axes[axisId]->setTicker(QSharedPointer<SuffixTicker>::create(scale, suffix, fixedStep));
}

void ChartView::setHoverReadout(bool on)
{
    d_->hoverOn = on;
    if (on && !d_->crosshair && d_->keyAxis) {
        QCustomPlot* p = d_->plot;
        d_->crosshair = new QCPItemLine(p);
        QPen cp(QColor(150, 150, 150, 160)); cp.setWidth(1);
        d_->crosshair->setPen(cp);
        d_->crosshair->start->setTypeX(QCPItemPosition::ptPlotCoords);
        d_->crosshair->end->setTypeX(QCPItemPosition::ptPlotCoords);
        d_->crosshair->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
        d_->crosshair->end->setTypeY(QCPItemPosition::ptAxisRectRatio);
        d_->crosshair->start->setAxes(d_->keyAxis, nullptr);
        d_->crosshair->end->setAxes(d_->keyAxis, nullptr);
        d_->crosshair->setVisible(false);
        d_->crosshair->setLayer("overlay");

        // Floating readout: a rich-text QLabel child of the plot so it can show
        // per-series colours and a rounded, semi-transparent box, then follows the
        // cursor (a single QCPItemText can only render one colour).
        d_->tooltip = new QLabel(p);
        d_->tooltip->setTextFormat(Qt::RichText);
        d_->tooltip->setAttribute(Qt::WA_TransparentForMouseEvents);
        d_->tooltip->setStyleSheet(
            "background: rgba(20,20,20,210); border: 1px solid rgba(150,150,150,90);"
            "border-radius: 4px; padding: 5px 8px;");
        d_->tooltip->hide();

        p->setMouseTracking(true);
        connect(p, &QCustomPlot::mouseMove, this, [this](QMouseEvent* e) {
            if (!d_->hoverOn || !d_->keyAxis) return;
            const QRect rect = d_->plot->axisRect()->rect();
            if (!rect.contains(e->pos())) {
                d_->crosshair->setVisible(false);
                d_->tooltip->hide();
                requestReplot();
                return;
            }
            const double key = d_->keyAxis->pixelToCoord(e->pos().x());
            d_->crosshair->start->setCoords(key, 0);
            d_->crosshair->end->setCoords(key, 1);
            d_->crosshair->setVisible(true);

            // Time header (m:ss), then one coloured line per named visible series.
            const int totalSec = qMax(0, (int)(key + 0.5));
            const QColor t = palette().color(QPalette::Text);
            QString html = QString("<div style='color:rgba(%1,%2,%3,0.6)'>%4:%5</div>")
                .arg(t.red()).arg(t.green()).arg(t.blue())
                .arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
            const QLocale loc;
            for (int i = 0; i < d_->graphs.size(); ++i) {
                QCPGraph* g = d_->graphs[i];
                if (!g->visible() || g->name().isEmpty() || g->data()->isEmpty()) continue;
                auto it = g->data()->findBegin(key);
                if (it == g->data()->constEnd()) --it;
                const Impl::SeriesMeta& m = d_->meta[i];
                QString val = m.group ? loc.toString(it->value, 'f', m.precision)
                                      : QString::number(it->value, 'f', m.precision);
                if (!m.unit.isEmpty()) val += (m.unit == "%" ? "" : " ") + m.unit;
                html += QString("<div style='color:%1'><b>%2:</b> %3</div>")
                    .arg(g->pen().color().name(), g->name(), val);
            }
            d_->tooltip->setText(html);
            d_->tooltip->adjustSize();

            // Offset from the cursor; flip left/up near the right/bottom edges.
            const int pad = 14;
            QPoint pos = e->pos() + QPoint(pad, pad);
            if (pos.x() + d_->tooltip->width() > d_->plot->width())
                pos.setX(e->pos().x() - pad - d_->tooltip->width());
            if (pos.y() + d_->tooltip->height() > d_->plot->height())
                pos.setY(e->pos().y() - pad - d_->tooltip->height());
            d_->tooltip->move(pos);
            d_->tooltip->show();
            d_->tooltip->raise();
            requestReplot();
        });
    }
    if (d_->crosshair) d_->crosshair->setVisible(false);
    if (d_->tooltip)   d_->tooltip->hide();
}

bool ChartView::seriesKeyRange(int seriesId, double& lo, double& hi) const
{
    if (seriesId < 0 || seriesId >= d_->graphs.size()) return false;
    auto data = d_->graphs[seriesId]->data();
    if (data->isEmpty()) return false;
    bool found = false;
    const QCPRange r = data->keyRange(found);
    if (!found) return false;
    lo = r.lower;
    hi = r.upper;
    return true;
}

void ChartView::setXRange(int axisId, double min, double max)
{
    if (axisId < 0 || axisId >= d_->axes.size()) return;
    QCPAxis* ax = d_->axes[axisId];
    ax->setRange(min, max);

    // Only decimate when the visible window is wide enough to warrant it. Gating
    // on the shared key (x) axis keeps every series in lock-step; below 2 minutes
    // we keep all points, at/above we let QCustomPlot collapse to ~per-pixel.
    if (ax == d_->keyAxis) {
        const bool decimate = (max - min) >= DECIMATE_MIN_WINDOW_S;
        for (QCPGraph* g : d_->graphs)
            g->setAdaptiveSampling(decimate);
    }
}

void ChartView::requestReplot()
{
    // Queued: many updates within one event-loop turn collapse to a single paint.
    d_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ChartView::applyPaletteText()
{
    const QColor text = palette().color(QPalette::Text);
    for (int i = 0; i < d_->axes.size(); ++i) {
        if (d_->axisInheritColor[i]) {
            d_->axes[i]->setTickLabelColor(text);
            d_->axes[i]->setLabelColor(text);
        }
    }
    d_->plot->legend->setTextColor(text);
}

void ChartView::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange ||
        e->type() == QEvent::ApplicationPaletteChange) {
        applyPaletteText();
        requestReplot();
    }
}
