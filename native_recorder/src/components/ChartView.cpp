#include "ChartView.h"

#include "../third_party/qcustomplot/qcustomplot.h"

#include <QEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

// All QCustomPlot specifics live here, behind ChartView's backend-agnostic API.
struct ChartView::Impl {
    QCustomPlot*         plot = nullptr;
    QVector<QCPAxis*>    axes;            // by axis id
    QVector<bool>        axisInheritColor;
    QVector<QCPGraph*>   graphs;          // by series id

    QCPAxis*      keyAxis   = nullptr;    // first Bottom axis — the shared x
    bool          hoverOn   = false;
    QCPItemLine*  crosshair = nullptr;
    QCPItemText*  readout   = nullptr;

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
    p->setBackground(Qt::NoBrush);
    p->setBackground(QBrush(Qt::transparent));
    p->axisRect()->setMargins(QMargins(0, 0, 0, 0));
    p->axisRect()->setAutoMargins(QCP::msNone);

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
    ax->grid()->setVisible(false);
    ax->setNumberFormat(QString(QChar(spec.numberFormat)));
    ax->setNumberPrecision(spec.precision);

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
    g->setAdaptiveSampling(true);   // per-pixel decimation — flat paint cost

    const int id = d_->graphs.size();
    d_->graphs.append(g);
    return id;
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

void ChartView::setAxisTimeTicker(int axisId, const QString& format)
{
    if (axisId < 0 || axisId >= d_->axes.size()) return;
    auto ticker = QSharedPointer<QCPAxisTickerTime>::create();
    ticker->setTimeFormat(format);
    d_->axes[axisId]->setTicker(ticker);
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

        d_->readout = new QCPItemText(p);
        d_->readout->setPositionAlignment(Qt::AlignTop | Qt::AlignLeft);
        d_->readout->position->setType(QCPItemPosition::ptAxisRectRatio);
        d_->readout->position->setCoords(0.01, 0.02);
        d_->readout->setTextAlignment(Qt::AlignLeft);
        d_->readout->setPadding(QMargins(4, 2, 4, 2));
        d_->readout->setColor(palette().color(QPalette::Text));
        d_->readout->setBrush(QBrush(QColor(0, 0, 0, 90)));
        d_->readout->setPen(Qt::NoPen);
        d_->readout->setVisible(false);
        d_->readout->setLayer("overlay");

        p->setMouseTracking(true);
        connect(p, &QCustomPlot::mouseMove, this, [this](QMouseEvent* e) {
            if (!d_->hoverOn || !d_->keyAxis) return;
            const QRect rect = d_->plot->axisRect()->rect();
            if (!rect.contains(e->pos())) {
                d_->crosshair->setVisible(false);
                d_->readout->setVisible(false);
                requestReplot();
                return;
            }
            const double key = d_->keyAxis->pixelToCoord(e->pos().x());
            d_->crosshair->start->setCoords(key, 0);
            d_->crosshair->end->setCoords(key, 1);
            d_->crosshair->setVisible(true);

            // x as m:ss.s
            const int totalTenths = qMax(0, (int)(key * 10 + 0.5));
            QString txt = QString("%1:%2.%3")
                .arg(totalTenths / 600)
                .arg((totalTenths / 10) % 60, 2, 10, QChar('0'))
                .arg(totalTenths % 10);
            for (QCPGraph* g : d_->graphs) {
                if (!g->visible() || g->data()->isEmpty()) continue;
                auto it = g->data()->findBegin(key);
                if (it == g->data()->constEnd()) --it;
                txt += QString("   %1 %2")
                    .arg(g->name().isEmpty() ? QStringLiteral("·") : g->name())
                    .arg(qRound(it->value));
            }
            d_->readout->setText(txt);
            d_->readout->setColor(palette().color(QPalette::Text));
            d_->readout->setVisible(true);
            requestReplot();
        });
    }
    if (d_->crosshair) { d_->crosshair->setVisible(false); d_->readout->setVisible(false); }
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
    d_->axes[axisId]->setRange(min, max);
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
