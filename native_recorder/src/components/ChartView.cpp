#include "ChartView.h"
#include "ChartViewModel.h"

#include <QEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

// QCustomPlot is gone — ChartView now hosts a Qt Graphs (2D) scene in a
// QQuickWidget and delegates every call to ChartViewModel, which owns the series,
// axes and raw sample store. Rendering goes through the Qt Quick scene graph on
// Qt's RHI (Vulkan/Direct3D/Metal — see main.cpp), and the decoration Qt Graphs
// can't draw (custom tick labels, crosshair, tooltip, legend, bands) is rendered
// by ChartSurface.qml bound to the view model. The public API is unchanged, so no
// chart subclass or caller needed touching.
struct ChartView::Impl {
    QQuickWidget*   quick = nullptr;
    ChartViewModel* vm    = nullptr;
    bool            replotQueued = false;
};

ChartView::ChartView(QWidget* parent)
    : QWidget(parent), d_(std::make_unique<Impl>())
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(120);

    d_->vm = new ChartViewModel(this);

    d_->quick = new QQuickWidget(this);
    d_->quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    // Paint the chart's clear colour with the window colour (opaque) rather than
    // making the widget transparent. A transparent QQuickWidget needs
    // WA_AlwaysStackOnTop to composite, which forced the chart above every sibling
    // and made the series draw over overlays like "Loading recording…". An opaque
    // widget the same colour as the window looks identical but stacks normally.
    // (Kept in sync with the theme in applyPaletteText.)
    d_->quick->setClearColor(palette().color(QPalette::Window));
    d_->quick->rootContext()->setContextProperty("vm", d_->vm);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(d_->quick);

    // QQuickWidget loads synchronously, so the root object and its GraphsView are
    // available right after setSource — before any subclass constructor adds axes.
    d_->quick->setSource(QUrl("qrc:/qml/ChartSurface.qml"));
    if (auto* root = d_->quick->rootObject()) {
        if (auto* gv = root->findChild<QQuickItem*>("graphsView"))
            d_->vm->attach(gv);
    }

    applyPaletteText();
}

ChartView::~ChartView() = default;

int  ChartView::addAxis(const AxisSpec& spec)   { return d_->vm->addAxis(spec); }
int  ChartView::addSeries(const SeriesSpec& spec) { return d_->vm->addSeries(spec); }
void ChartView::addBand(const BandSpec& spec)   { d_->vm->addBand(spec); }

void ChartView::appendPoint(int seriesId, double x, double y) { d_->vm->appendPoint(seriesId, x, y); }

void ChartView::setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys)
{
    d_->vm->setSeriesData(seriesId, xs, ys);
}

void ChartView::trimBefore(int seriesId, double x) { d_->vm->trimBefore(seriesId, x); }
void ChartView::clear(int seriesId)                { d_->vm->clear(seriesId); }
void ChartView::clearAll()                         { d_->vm->clearAll(); }
void ChartView::setSeriesVisible(int seriesId, bool visible) { d_->vm->setSeriesVisible(seriesId, visible); }

void ChartView::setAxisTimeTicker(int axisId, const QString& /*format*/) { d_->vm->setAxisTimeTicker(axisId); }

void ChartView::setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep)
{
    d_->vm->setAxisNumberSuffix(axisId, scale, suffix, fixedStep);
}

void ChartView::setLegendVisible(bool on)   { d_->vm->setLegendVisible(on); }
void ChartView::setHoverReadout(bool on)    { d_->vm->setHoverReadout(on); }

bool ChartView::seriesKeyRange(int seriesId, double& lo, double& hi) const
{
    return d_->vm->seriesKeyRange(seriesId, lo, hi);
}

void ChartView::setXRange(int axisId, double min, double max) { d_->vm->setXRange(axisId, min, max); }

void ChartView::requestReplot()
{
    // Coalesce: collapse many updates in one event-loop turn into a single flush
    // (which decimates the raw data to the current pixel width and pushes it to
    // the line series).
    if (d_->replotQueued) return;
    d_->replotQueued = true;
    QTimer::singleShot(0, this, [this] {
        d_->replotQueued = false;
        d_->vm->flush(d_->quick ? d_->quick->width() : width());
    });
}

void ChartView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    requestReplot();   // re-decimate to the new pixel width
}

void ChartView::applyPaletteText()
{
    d_->vm->setThemeColors(palette().color(QPalette::Text));
    if (d_->quick) d_->quick->setClearColor(palette().color(QPalette::Window));
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
