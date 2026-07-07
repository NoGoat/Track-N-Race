#include "ChartView.h"

#include "../third_party/qcustomplot/qcustomplot.h"

#include <QEvent>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QSet>
#include <QSettings>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

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

// Gap left between panels (see layoutPanelsRows); dividers are drawn down its
// centre so they never crowd a panel's axis labels.
constexpr int PANEL_GAP = ChartView::PanelGap;

// Minimum right margin reserved on each multi-panel axis rect so the last x-axis
// tick label (centred on the right edge) stays inside the panel instead of
// spilling into the gap where the divider sits.
constexpr int PANEL_LABEL_CLEAR = 24;

// A whole-plot overlay that draws the divider lines between panels. Drawing them
// as one overlay (rather than per-cell layout elements) keeps every line the same
// crisp width, centres them in the inter-panel gaps, and lets horizontals and
// verticals cross and meet cleanly. The geometry lives in ChartView (which owns
// the panels), so the actual drawing is injected as a callback.
class DividerOverlay : public QCPLayerable {
public:
    DividerOverlay(QCustomPlot* plot, std::function<void(QCPPainter*)> fn)
        : QCPLayerable(plot, QStringLiteral("axes")), fn_(std::move(fn)) {}
protected:
    void applyDefaultAntialiasingHint(QCPPainter*) const override {}
    void draw(QCPPainter* painter) override { if (fn_) fn_(painter); }
    std::function<void(QCPPainter*)> fn_;
};
}

// All QCustomPlot specifics live here, behind ChartView's backend-agnostic API.
struct ChartView::Impl {
    // Per-series tooltip metadata, parallel to graphs.
    struct SeriesMeta { QString unit; int precision = 0; bool group = false; };

    // One chart within the shared plot. Panel 0 wraps QCustomPlot's default axis
    // rect/legend so single-panel charts behave exactly as before; addPanel() adds
    // more. Each panel owns its key (x) axis, colour-key legend, title and hover
    // crosshair; the value tooltip (below) is shared across panels. When it gets a
    // header (title+legend row), the rect is wrapped in a 2-row container grid so
    // the header sits *above* the plot area rather than overlaying it.
    struct Panel {
        QCPAxisRect*    rect      = nullptr;
        QCPLayoutGrid*  container = nullptr;   // header row + rect (nullptr if no header)
        QCPAxis*        key       = nullptr;   // this panel's Bottom (shared x) axis
        QCPLegend*      legend    = nullptr;
        QCPTextElement* title     = nullptr;   // created by setPanelTitle/ensurePanelHeader
        QCPItemLine*    crosshair = nullptr;   // lazily created by setHoverReadout
        bool            visible   = true;

        // Top-level layout element for this panel: the header container if built,
        // else the bare axis rect.
        QCPLayoutElement* element() const {
            return container ? static_cast<QCPLayoutElement*>(container)
                             : static_cast<QCPLayoutElement*>(rect);
        }
    };

    QCustomPlot*         plot = nullptr;
    QVector<QCPAxis*>    axes;            // by axis id
    QVector<bool>        axisInheritColor;
    QVector<QCPGraph*>   graphs;          // by series id
    QVector<SeriesMeta>  meta;            // by series id

    QVector<Panel>       panels;          // panels[0] == default rect/legend
    int                  panelCols = 1;   // grid columns (see layoutPanels)
    QVector<QCPLayoutGrid*>  rowGrids;    // nested per-row grids from layoutPanelsRows
    QVector<QCPLayoutElement*> spacers;   // blank cells reserved for overlaid widgets
    QVector<QVector<QCPLayoutElement*>> layoutCells;  // element per placed cell (for dividers)
    DividerOverlay*      dividerOverlay = nullptr;              // draws lines between panels
    QColor               dividerColor{ 128, 128, 128, 120 };   // theme-updated in applyPaletteText

    QCPAxis*      keyAxis   = nullptr;    // panel 0's Bottom axis (alias for addBand)
    bool          hoverOn   = false;
    QLabel*       tooltip   = nullptr;    // floating, cursor-following value readout

    // Panel index owning a given rect/axis (falls back to 0 — the default panel).
    int panelOf(QCPAxisRect* r) const {
        for (int i = 0; i < panels.size(); ++i) if (panels[i].rect == r) return i;
        return 0;
    }

    // FPS / replot-timing diagnostics (opt-in, see constructor).
    QElapsedTimer fpsFrameTimer;          // spans one beforeReplot→afterReplot
    QElapsedTimer fpsWindow;              // the current ~1s reporting window
    int           fpsCount   = 0;         // replots completed this window
    double        fpsAccumMs = 0.0;       // summed render time this window
    double        fpsLastMs  = 0.0;       // most recent replot's render time

    static QCPAxis::AxisType toQcp(Side s) {
        switch (s) {
            case Side::Bottom: return QCPAxis::atBottom;
            case Side::Left:   return QCPAxis::atLeft;
            case Side::Right:  return QCPAxis::atRight;
        }
        return QCPAxis::atLeft;
    }
};

namespace {
// Every live ChartView, so a settings change can be pushed to all of them at
// once (see ChartView::reapplyRenderSettings). Charts add/remove themselves in
// their constructor/destructor.
QSet<ChartView*>& liveCharts() { static QSet<ChartView*> set; return set; }

// Chart GPU-render settings live in the shared app QSettings so ChartView needn't
// depend on MainWindow. Default: 4x MSAA — smooth thin lines at a fraction of the
// 16x fill cost. 0 means anti-aliasing off entirely (fastest, aliased lines).
int readMsaaSamples() {
    QSettings s("TrackNRace", "NativeRecorder");
    const int n = s.value("ui/chartMsaaSamples", 4).toInt();
    return (n == 0 || n == 2 || n == 4 || n == 8 || n == 16) ? n : 4;   // clamp to offered set
}

// Style a legend as a compact single-row colour key (FL FR RL RR …): no frame,
// transparent, small font — matching the old per-section header swatches.
void styleKeyLegend(QCPLegend* leg) {
    leg->setLayer("legend");
    leg->setBrush(QBrush(Qt::transparent));
    leg->setBorderPen(Qt::NoPen);
    leg->setFillOrder(QCPLegend::foColumnsFirst);
    leg->setWrap(4);                       // fill columns first, wrap after 4 → one row
    leg->setRowSpacing(0);
    leg->setColumnSpacing(6);
    QFont f = leg->font(); f.setPointSize(8); leg->setFont(f);
    // Match the icon height to the text line height: QCustomPlot top-aligns an icon
    // shorter than the text, so a short icon reads as floating above the label. The
    // colour sample is drawn centered within the icon rect, so equal heights center
    // it against the text (and item height was already the text height, so no growth).
    leg->setIconSize(14, QFontMetrics(f).height());
    leg->setMargins(QMargins(0, 0, 0, 0));
}

// Apply the render settings to one plot. setOpenGl() recreates the paint buffers
// with the new sample count, so this is safe to call at runtime for a live change.
void applyRenderSettings(QCustomPlot* p) {
    const int samples = readMsaaSamples();
#ifdef QCUSTOMPLOT_USE_OPENGL
    p->setOpenGl(true, samples);   // sets the aeAll antialiasing override on success
#endif
    // "Off" (0 samples): also drop QCustomPlot's antialiasing override so lines
    // render aliased (cheapest). Any >0 count restores full AA — setOpenGl re-applies
    // aeAll above, and for non-GL builds we set it explicitly so an off→on switch
    // isn't sticky.
    p->setAntialiasedElements(samples == 0 ? QCP::aeNone : QCP::aeAll);
}
}  // namespace

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

    // GPU rasterization (OpenGL), with the MSAA level read from the user's
    // settings (Appearance tab). The software QPainter path costs ~90 ms/frame
    // here, and seconds/frame at a 10-min window; OpenGL renders the same content
    // in single-digit ms. MSAA trades fill cost for line smoothness.
    applyRenderSettings(p);
    liveCharts().insert(this);

#ifdef QCUSTOMPLOT_USE_OPENGL
    // openGl() reflects whether QCustomPlot *kept* the GL path on: setOpenGl()
    // quietly leaves it disabled if the GL context/extensions aren't usable, so
    // this is the per-chart ground truth (vs. the compile-time request above).
    qInfo("[opengl] ChartView %p: OpenGL requested (%dx MSAA); QCustomPlot openGl()=%s",
          static_cast<void*>(this), readMsaaSamples(),
          p->openGl() ? "true (GPU)" : "false (software fallback)");
#else
    qInfo("[opengl] ChartView %p: built without OpenGL - software QPainter rendering",
          static_cast<void*>(this));
#endif

    // FPS / replot-timing diagnostics. A Qt Widgets app has no fixed render loop;
    // charts repaint on demand, so "FPS" here is the chart's replot rate — exactly
    // what the OpenGL path accelerates. Opt-in via TNR_FPS=1 so normal runs stay
    // quiet. We time each replot (beforeReplot→afterReplot) and, once per second,
    // log this chart's replot count and mean/last render time. Labelled by the
    // concrete subclass (GForceChart, PowerChart, …) so charts are told apart.
    if (qEnvironmentVariableIntValue("TNR_FPS") > 0) {
        connect(p, &QCustomPlot::beforeReplot, this, [this]() {
            d_->fpsFrameTimer.restart();
        });
        connect(p, &QCustomPlot::afterReplot, this, [this]() {
            const double ms = d_->fpsFrameTimer.nsecsElapsed() / 1.0e6;
            d_->fpsAccumMs += ms;
            d_->fpsLastMs = ms;
            ++d_->fpsCount;
            const qint64 winMs = d_->fpsWindow.elapsed();
            if (winMs >= 1000) {
                qInfo("[fps] %-14s %.1f replots/s  avg %.2f ms  last %.2f ms",
                      metaObject()->className(),
                      d_->fpsCount * 1000.0 / winMs,
                      d_->fpsAccumMs / d_->fpsCount,
                      d_->fpsLastMs);
                d_->fpsCount = 0;
                d_->fpsAccumMs = 0.0;
                d_->fpsWindow.restart();
            }
        });
        d_->fpsWindow.start();
    }

    // Opaque background (theme's Window colour, set in applyPaletteText) rather
    // than transparent: a transparent chart forces the GPU to alpha-composite the
    // whole rect against what's behind it every replot; an opaque fill skips that.
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

    // Panel 0 wraps the plot's built-in axis rect + legend, so single-panel charts
    // are unchanged; addPanel() appends more rects to this same backend/GL context.
    // Fields: rect, container, key, legend, title, crosshair, visible.
    d_->panels.append(Impl::Panel{ p->axisRect(), nullptr, nullptr, p->legend, nullptr, nullptr, true });

    // Overlay that draws divider lines between panels (multi-panel charts only; it
    // draws nothing until layoutPanelsRows records a layout). Owned by the plot.
    d_->dividerOverlay = new DividerOverlay(p, [d = d_.get()](QCPPainter* painter) {
        const int N = d->layoutCells.size();
        if (N == 0) return;
        painter->setAntialiasing(false);
        QPen pen(d->dividerColor); pen.setCosmetic(true);
        painter->setPen(pen);
        const int half = PANEL_GAP / 2;
        // Cells hold the placed layout element (a panel container or a blank spacer
        // reserved for an overlaid table), so a row splits its width evenly and
        // dividers line up across rows; the gap between cells is PANEL_GAP.
        auto rectOf = [](QCPLayoutElement* e) { return e->rect(); };

        // Vertical dividers between adjacent cells in a row. Extend into the gaps
        // above/below (half a gap each) so they meet the horizontal dividers.
        for (int ri = 0; ri < N; ++ri) {
            const QVector<QCPLayoutElement*>& row = d->layoutCells[ri];
            if (row.size() < 2) continue;
            int top = rectOf(row[0]).top(), bot = rectOf(row[0]).bottom();
            for (auto* e : row) { const QRect r = rectOf(e); top = qMin(top, r.top()); bot = qMax(bot, r.bottom()); }
            if (ri > 0)     top -= half;
            if (ri < N - 1) bot += half;
            for (int k = 1; k < row.size(); ++k) {
                const int x = (rectOf(row[k - 1]).right() + rectOf(row[k]).left()) / 2;
                painter->drawLine(QPointF(x, top), QPointF(x, bot));
            }
        }
        // Horizontal dividers between consecutive rows, spanning their combined width.
        for (int ri = 1; ri < N; ++ri) {
            const QVector<QCPLayoutElement*>& a = d->layoutCells[ri - 1];
            const QVector<QCPLayoutElement*>& b = d->layoutCells[ri];
            int aboveBot = rectOf(a[0]).bottom(), belowTop = rectOf(b[0]).top();
            int left = rectOf(a[0]).left(), right = rectOf(a[0]).right();
            for (auto* e : a) { const QRect r = rectOf(e); aboveBot = qMax(aboveBot, r.bottom()); left = qMin(left, r.left()); right = qMax(right, r.right()); }
            for (auto* e : b) { const QRect r = rectOf(e); belowTop = qMin(belowTop, r.top());   left = qMin(left, r.left()); right = qMax(right, r.right()); }
            const int y = (aboveBot + belowTop) / 2;
            painter->drawLine(QPointF(left, y), QPointF(right, y));
        }
    });

    applyPaletteText();
}

ChartView::~ChartView() { liveCharts().remove(this); }

void ChartView::reapplyRenderSettings()
{
    for (ChartView* v : liveCharts()) {
        applyRenderSettings(v->d_->plot);
        v->d_->plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

int ChartView::addAxis(const AxisSpec& spec, int panelId)
{
    if (panelId < 0 || panelId >= d_->panels.size()) panelId = 0;
    QCPAxis* ax = d_->panels[panelId].rect->addAxis(Impl::toQcp(spec.side));
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
    if (spec.side == Side::Bottom) {
        if (!d_->panels[panelId].key) d_->panels[panelId].key = ax;   // this panel's x
        if (panelId == 0 && !d_->keyAxis) d_->keyAxis = ax;           // addBand alias
    }
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

    // Route the graph to its owning panel's colour-key legend (addGraph auto-joins
    // the default one). Unnamed series (e.g. muted reference traces) stay out of it.
    const int panel = d_->panelOf(vax->axisRect());
    g->removeFromLegend();
    if (!spec.name.isEmpty() && d_->panels[panel].legend)
        g->addToLegend(d_->panels[panel].legend);

    // Decimation is gated on the current window width (see setXRange); a new series
    // inherits whatever its panel's key axis already implies.
    QCPAxis* pkey = d_->panels[panel].key;
    const double window = pkey ? pkey->range().size() : 0.0;
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

int ChartView::addPanel()
{
    QCPAxisRect* rect = new QCPAxisRect(d_->plot, /*setupDefaultAxes=*/false);
    rect->setAutoMargins(QCP::msAll);

    Impl::Panel panel;
    panel.rect = rect;
    const int id = d_->panels.size();
    d_->panels.append(panel);
    // Not placed until layoutPanels() (or a header is built); see applyPanelLayout.
    return id;
}

void ChartView::ensurePanelHeader(int panelId)
{
    Impl::Panel& pn = d_->panels[panelId];
    if (pn.container) return;   // header already built

    QCustomPlot* p = d_->plot;

    // A dedicated compact colour key for this panel. Panel 0 starts out pointing at
    // the plot's built-in centred legend; retire it in favour of our own.
    if (pn.legend == p->legend) p->legend->setVisible(false);
    pn.legend = new QCPLegend;
    styleKeyLegend(pn.legend);

    pn.title = new QCPTextElement(p, QString());
    pn.title->setLayer("legend");
    { QFont f = pn.title->font(); f.setPointSize(8); f.setBold(true); pn.title->setFont(f); }
    pn.title->setTextColor(palette().color(QPalette::PlaceholderText));
    pn.title->setTextFlags(Qt::AlignLeft | Qt::AlignVCenter);
    pn.title->setMargins(QMargins(0, 0, 0, 0));

    // Header row: title (left, absorbs slack) | colour key (right).
    QCPLayoutGrid* header = new QCPLayoutGrid;
    header->setMargins(QMargins(8, 4, 8, 3));
    header->setColumnSpacing(6);
    header->addElement(0, 0, pn.title);
    header->addElement(0, 1, pn.legend);
    header->setColumnStretchFactor(0, 1);
    header->setColumnStretchFactor(1, 0.001);
    header->setMaximumSize(QWIDGETSIZE_MAX, 26);   // keep the bar thin, like the header strip

    // Reserve enough right margin that the last x-axis tick label stays inside the
    // panel rather than spilling into the divider gap next to it.
    pn.rect->setMinimumMargins(QMargins(0, 0, PANEL_LABEL_CLEAR, 0));

    // Container: header on top, the axis rect below (which takes the slack). A row
    // gap keeps the title clear of the plot's top tick label.
    QCPLayoutGrid* container = new QCPLayoutGrid;
    container->setMargins(QMargins(0, 0, 0, 0));
    container->setRowSpacing(10);
    p->plotLayout()->take(pn.rect);   // pull the rect out of the top grid if it's there
    p->plotLayout()->simplify();
    container->addElement(0, 0, header);
    container->addElement(1, 0, pn.rect);
    container->setRowStretchFactor(0, 0.001);
    container->setRowStretchFactor(1, 1);
    pn.container = container;

    applyPanelLayout();
}

void ChartView::layoutPanels(int columns)
{
    d_->panelCols = qMax(1, columns);
    applyPanelLayout();
}

void ChartView::layoutPanelsRows(const QVector<QVector<int>>& rows)
{
    QCPLayoutGrid* top = d_->plot->plotLayout();

    // Detach every panel element from whatever grid currently holds it (the top
    // grid, or a row sub-grid from a previous call), so we can freely re-place.
    for (const Impl::Panel& pn : d_->panels)
        if (QCPLayout* parent = pn.element()->layout()) parent->take(pn.element());

    // Drop the blank spacer cells from the previous call (they aren't panels, so
    // they're safe to delete — panels were detached above and survive).
    for (QCPLayoutElement* sp : d_->spacers) {
        if (QCPLayout* parent = sp->layout()) parent->take(sp);
        delete sp;
    }
    d_->spacers.clear();

    // Drop the (now-empty) row sub-grids from the previous call.
    for (QCPLayoutGrid* g : d_->rowGrids) { top->take(g); delete g; }
    d_->rowGrids.clear();
    top->simplify();
    top->setRowSpacing(PANEL_GAP);   // gaps between rows; dividers drawn down the centre
    // No outer margin, so panels (and the dividers spanning them) reach the widget
    // edges — where a page's own horizontal separators sit (e.g. the Overview tyre
    // strip), so the vertical dividers meet them.
    top->setMargins(QMargins(0, 0, 0, 0));

    // Hide every panel up front; the ones we place below are re-shown. A panel
    // dropped from the layout keeps its frozen geometry and would keep drawing
    // (layerables render via layers, not the layout tree) unless made invisible —
    // setVisible cascades to the panel's header, axes, graphs and items.
    for (const Impl::Panel& pn : d_->panels) pn.element()->setVisible(false);

    // Top grid is a single column, so a row holding one element spans the full
    // width; a row with several panels gets a nested horizontal sub-grid. The
    // DividerOverlay draws the lines between them, in the gaps left by the spacing.
    d_->layoutCells.clear();
    int topRow = 0;
    for (const QVector<int>& row : rows) {
        // Map each id to a layout element: a real panel, or a fresh blank spacer for
        // a negative id (a hole where the caller overlays an external widget).
        QVector<QCPLayoutElement*> cells;
        for (int id : row) {
            if (id >= 0 && id < d_->panels.size()) {
                d_->panels[id].element()->setVisible(true);
                cells.append(d_->panels[id].element());
            } else {
                auto* sp = new QCPLayoutElement(d_->plot);
                sp->setMargins(QMargins(0, 0, 0, 0));
                d_->spacers.append(sp);
                cells.append(sp);
            }
        }
        if (cells.isEmpty()) continue;

        if (cells.size() == 1) {
            top->addElement(topRow++, 0, cells[0]);
        } else {
            QCPLayoutGrid* rg = new QCPLayoutGrid;
            rg->setMargins(QMargins(0, 0, 0, 0));
            rg->setColumnSpacing(PANEL_GAP);
            for (int c = 0; c < cells.size(); ++c)
                rg->addElement(0, c, cells[c]);
            d_->rowGrids.append(rg);
            top->addElement(topRow++, 0, rg);
        }
        d_->layoutCells.append(cells);
    }

    top->simplify();
    d_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ChartView::applyPanelLayout()
{
    QCPLayoutGrid* grid = d_->plot->plotLayout();
    // Pull every panel (its header container or bare rect) out without deleting,
    // then re-place the visible ones row-major. simplify() collapses empty cells.
    for (const Impl::Panel& pn : d_->panels) grid->take(pn.element());
    grid->simplify();
    int idx = 0;
    for (const Impl::Panel& pn : d_->panels) {
        // A taken-out element keeps its last geometry and would keep drawing
        // (layerables render via the layer system, not the layout tree), so hidden
        // panels must be made invisible too. setVisible cascades to the panel's
        // header, axes, graphs and items through realVisibility().
        pn.element()->setVisible(pn.visible);
        if (!pn.visible) continue;
        grid->addElement(idx / d_->panelCols, idx % d_->panelCols, pn.element());
        ++idx;
    }
    grid->simplify();
    d_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ChartView::setPanelVisible(int panelId, bool on)
{
    if (panelId < 0 || panelId >= d_->panels.size()) return;
    if (d_->panels[panelId].visible == on) return;
    d_->panels[panelId].visible = on;
    applyPanelLayout();
}

void ChartView::setPanelTitle(int panelId, const QString& title)
{
    if (panelId < 0 || panelId >= d_->panels.size()) return;
    ensurePanelHeader(panelId);
    d_->panels[panelId].title->setText(title);
    d_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ChartView::setPanelLegendVisible(int panelId, bool on)
{
    if (panelId < 0 || panelId >= d_->panels.size()) return;
    ensurePanelHeader(panelId);
    d_->panels[panelId].legend->setVisible(on);
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

    // First-time setup: a crosshair per panel (confined to that panel's rect) plus
    // one shared floating value readout. Enable hover *after* panels/axes exist.
    if (on && !d_->tooltip) {
        QCustomPlot* p = d_->plot;

        for (Impl::Panel& panel : d_->panels) {
            if (panel.crosshair || !panel.key) continue;
            QCPItemLine* ch = new QCPItemLine(p);
            QPen cp(QColor(150, 150, 150, 160)); cp.setWidth(1);
            ch->setPen(cp);
            ch->start->setTypeX(QCPItemPosition::ptPlotCoords);
            ch->end->setTypeX(QCPItemPosition::ptPlotCoords);
            ch->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
            ch->end->setTypeY(QCPItemPosition::ptAxisRectRatio);
            ch->start->setAxes(panel.key, nullptr);
            ch->end->setAxes(panel.key, nullptr);
            ch->start->setAxisRect(panel.rect);   // ratio-Y spans only this panel
            ch->end->setAxisRect(panel.rect);
            ch->setClipAxisRect(panel.rect);
            ch->setClipToAxisRect(true);
            ch->setVisible(false);
            ch->setLayer("overlay");
            panel.crosshair = ch;
        }

        // Floating readout: a rich-text QLabel child of the plot so it can show
        // per-series colours and a rounded, semi-transparent box, then follows the
        // cursor (a single QCPItemText can only render one colour).
        d_->tooltip = new QLabel(p);
        d_->tooltip->setTextFormat(Qt::RichText);
        d_->tooltip->setAttribute(Qt::WA_TransparentForMouseEvents);
        {
            const auto rgba = [](const QColor& c) {
                return QString("rgba(%1,%2,%3,%4)")
                    .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
            };
            const QColor tipBg = palette().color(QPalette::Button);
            const QColor tipFg = palette().color(QPalette::ToolTipText);
            QColor tipBorder = tipFg; tipBorder.setAlpha(90);
            d_->tooltip->setStyleSheet(QString(
                "background: %1; color: %2; border: 1px solid %3; padding: 5px 8px;")
                .arg(rgba(tipBg), rgba(tipFg), rgba(tipBorder)));
        }
        d_->tooltip->hide();

        p->setMouseTracking(true);
        connect(p, &QCustomPlot::mouseMove, this, [this](QMouseEvent* e) {
            if (!d_->hoverOn) return;

            // Which panel is the cursor over? (Each panel is a separate axis rect.)
            int hp = -1;
            for (int i = 0; i < d_->panels.size(); ++i) {
                const Impl::Panel& pn = d_->panels[i];
                if (pn.visible && pn.key && pn.rect->rect().contains(e->pos())) { hp = i; break; }
            }
            for (Impl::Panel& pn : d_->panels)
                if (pn.crosshair) pn.crosshair->setVisible(false);
            if (hp < 0) { d_->tooltip->hide(); requestReplot(); return; }

            Impl::Panel& panel = d_->panels[hp];
            const double key = panel.key->pixelToCoord(e->pos().x());
            panel.crosshair->start->setCoords(key, 0);
            panel.crosshair->end->setCoords(key, 1);
            panel.crosshair->setVisible(true);

            // Time header (m:ss), then one coloured line per named visible series in
            // the hovered panel.
            const int totalSec = qMax(0, (int)(key + 0.5));
            const QColor t = palette().color(QPalette::ToolTipText);
            QString html = QString("<div style='color:rgba(%1,%2,%3,0.6)'>%4:%5</div>")
                .arg(t.red()).arg(t.green()).arg(t.blue())
                .arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
            const QLocale loc;
            for (int i = 0; i < d_->graphs.size(); ++i) {
                QCPGraph* g = d_->graphs[i];
                if (!g->keyAxis() || g->keyAxis()->axisRect() != panel.rect) continue;
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
    for (Impl::Panel& panel : d_->panels)
        if (panel.crosshair) panel.crosshair->setVisible(false);
    if (d_->tooltip) d_->tooltip->hide();
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
    // on a panel's key (x) axis keeps that panel's series in lock-step; below 2
    // minutes we keep all points, at/above we let QCustomPlot collapse to ~per-pixel.
    for (const Impl::Panel& pn : d_->panels) {
        if (ax != pn.key) continue;
        const bool decimate = (max - min) >= DECIMATE_MIN_WINDOW_S;
        for (QCPGraph* g : d_->graphs)
            if (g->keyAxis() && g->keyAxis()->axisRect() == pn.rect)
                g->setAdaptiveSampling(decimate);
        break;
    }
}

void ChartView::requestReplot()
{
    // Queued: many updates within one event-loop turn collapse to a single paint.
    d_->plot->replot(QCustomPlot::rpQueuedReplot);
}

void ChartView::applyPaletteText()
{
    // Opaque background in the theme's Window colour (see constructor) — kept here
    // so it re-applies on theme/palette changes alongside the text colours.
    d_->plot->setBackground(QBrush(palette().color(QPalette::Window)));

    const QColor text = palette().color(QPalette::Text);
    for (int i = 0; i < d_->axes.size(); ++i) {
        if (d_->axisInheritColor[i]) {
            d_->axes[i]->setTickLabelColor(text);
            d_->axes[i]->setLabelColor(text);
        }
    }
    // Per-panel colour-key legends follow Text; titles use the dimmer placeholder
    // colour (matching the old section headers). Panel 0's legend is plot->legend.
    const QColor titleColor = palette().color(QPalette::PlaceholderText);
    for (const Impl::Panel& pn : d_->panels) {
        if (pn.legend) pn.legend->setTextColor(text);
        if (pn.title)  pn.title->setTextColor(titleColor);
    }

    // Panel divider lines (drawn by DividerOverlay): ~28% from the window colour
    // toward text, the app's divider style, so they read in both light and dark.
    const QColor win = palette().color(QPalette::Window);
    d_->dividerColor = QColor(
        win.red()   + int((text.red()   - win.red())   * 0.28),
        win.green() + int((text.green() - win.green()) * 0.28),
        win.blue()  + int((text.blue()  - win.blue())  * 0.28));
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
