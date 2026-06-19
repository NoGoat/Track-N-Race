#include "ChartViewModel.h"
#include "ChartDecimate.h"

#include <QtGraphs/QLineSeries>
#include <QtGraphs/QAreaSeries>
#include <QtGraphs/QValueAxis>
#include <QtGraphs/QAbstractAxis>
#include <QtGraphs/QAbstractSeries>

#include <QQuickItem>
#include <QLocale>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
const QColor AXIS_LINE(150, 150, 150, 130);
const QColor GRID_LINE(150, 150, 150, 40);

// A "nice" 1/2/5×10^n step that yields roughly `target` ticks across `span`.
double niceStep(double span, int target = 6)
{
    if (span <= 0 || target <= 0) return 1.0;
    const double rough = span / target;
    const double mag = std::pow(10.0, std::floor(std::log10(rough)));
    const double norm = rough / mag;
    double nice = 10.0;
    if (norm < 1.5)      nice = 1.0;
    else if (norm < 3.0) nice = 2.0;
    else if (norm < 7.0) nice = 5.0;
    return nice * mag;
}
}

ChartViewModel::ChartViewModel(QObject* parent) : QObject(parent) {}
ChartViewModel::~ChartViewModel() = default;

QColor ChartViewModel::axisLineColor() const { return AXIS_LINE; }
QColor ChartViewModel::gridColor()     const { return GRID_LINE; }

void ChartViewModel::attach(QQuickItem* graphsView)
{
    view_ = graphsView;
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    // Size each visible axis's label area to its actual content, so native labels
    // (applyAxisLabelMode) don't force Qt's fixed ~60px reservation. (Qt 6.12+.)
    view_->setProperty("dynamicLabelMargins", true);
#endif
    // Any axes/series added before the QML scene was Ready get wired in now.
    for (const Axis& a : axes_) {
        if (a.isX && !gridXSet_) {
            view_->setProperty("axisX", QVariant::fromValue<QAbstractAxis*>(a.obj));
            gridXSet_ = true;
        }
        if (!a.isX && a.side == ChartView::Side::Left && !gridYSet_) {
            view_->setProperty("axisY", QVariant::fromValue<QAbstractAxis*>(a.obj));
            gridYSet_ = true;
        }
    }
    for (const Series& s : series_) {
        if (s.area) addSeriesToView(s.area);   // fill behind
        addSeriesToView(s.line);               // stroke on top
    }
    applyAxisLabelMode();
    buildLabels();
    updateMargins();
}

void ChartViewModel::addSeriesToView(QAbstractSeries* s)
{
    // QGraphsView::addSeries takes QObject* (it accepts any series type).
    if (view_ && s)
        QMetaObject::invokeMethod(view_, "addSeries", Q_ARG(QObject*, s));
}

int ChartViewModel::addAxis(const ChartView::AxisSpec& spec)
{
    Axis a;
    a.side = spec.side;
    a.isX  = (spec.side == ChartView::Side::Bottom);
    a.min  = spec.min;
    a.max  = spec.max;
    a.inheritColor = !spec.labelColor.isValid();
    a.color = spec.labelColor;
    a.grid  = spec.grid;
    a.precision = spec.precision;

    // Column among axes on the same side, so multiple right (or left) axes' labels
    // stack outward instead of overlapping at one x.
    for (const Axis& other : axes_)
        if (other.side == spec.side) ++a.sideIndex;

    auto* ax = new QValueAxis(this);
    ax->setMin(spec.min);
    ax->setMax(spec.max);
    ax->setGridVisible(spec.grid);
    ax->setSubGridVisible(false);
    // Axis visibility (and thus whether its labels are native or overlay) is decided
    // in applyAxisLabelMode() below. A visible axis reserves a fixed 60px label area
    // in Qt 6.11 (collapsed when invisible — see updateAxisAreas()); 6.12's
    // dynamicLabelMargins sizes that area to content, enabling tight native labels.
    // The grid keys off gridVisible (not visibility) and series map off the range,
    // so collapsing the area doesn't affect either.
    switch (spec.side) {
        case ChartView::Side::Bottom: ax->setAlignment(Qt::AlignBottom); break;
        case ChartView::Side::Left:   ax->setAlignment(Qt::AlignLeft);   break;
        case ChartView::Side::Right:  ax->setAlignment(Qt::AlignRight);  break;
    }
    a.obj = ax;

    const int id = axes_.size();
    axes_.append(a);
    if (a.isX && keyAxisId_ < 0) keyAxisId_ = id;
    if (spec.side == ChartView::Side::Left && primaryLeftId_ < 0) primaryLeftId_ = id;

    // The GraphsView needs a primary x/y axis to define the plot area + grid.
    if (view_) {
        if (a.isX && !gridXSet_) {
            view_->setProperty("axisX", QVariant::fromValue<QAbstractAxis*>(ax));
            gridXSet_ = true;
        }
        if (!a.isX && spec.side == ChartView::Side::Left && !gridYSet_) {
            view_->setProperty("axisY", QVariant::fromValue<QAbstractAxis*>(ax));
            gridYSet_ = true;
        }
    }
    applyAxisLabelMode();
    buildLabels();
    updateMargins();
    return id;
}

int ChartViewModel::addSeries(const ChartView::SeriesSpec& spec)
{
    Series s;
    s.name = spec.name;
    s.color = spec.color;
    s.unit = spec.unit;
    s.precision = spec.tipPrecision;
    s.group = spec.tipGroupThousands;
    s.xAxisId = spec.xAxisId;
    s.yAxisId = spec.yAxisId;

    s.fill = spec.fill;

    auto* line = new QLineSeries(this);
    line->setName(spec.name);
    line->setColor(spec.color);
    line->setWidth(spec.width);
    if (spec.step) line->setLineStyle(QLineSeries::LineStyle::StepLeft);
    s.line = line;

    QValueAxis* xax = (spec.xAxisId >= 0 && spec.xAxisId < axes_.size()) ? axes_[spec.xAxisId].obj : nullptr;
    QValueAxis* yax = (spec.yAxisId >= 0 && spec.yAxisId < axes_.size()) ? axes_[spec.yAxisId].obj : nullptr;
    // Axis attachment rule:
    //  - A series wholly on the GraphsView's primary x/y axes must INHERIT them.
    //    Re-associating a primary axis per-series triggers the "axis already
    //    associated" warning and corrupts the area-fill coordinate transform.
    //  - A series touching ANY secondary axis (e.g. TelemetryChart's right-hand
    //    RPM/ERS) is multi-axis and must set BOTH axes explicitly (the documented
    //    pattern); inheriting only one is unreliable. These are plain lines, never
    //    fills, so the benign primary-x re-association warning is harmless.
    const bool multiAxis = (xax && spec.xAxisId != keyAxisId_)
                        || (yax && spec.yAxisId != primaryLeftId_);
    const bool setX = multiAxis && xax;
    const bool setY = multiAxis && yax;

    if (spec.fill) {
        // Translucent fill with ONLY an upperSeries and NO lowerSeries: the area
        // renderer then fills the curve down to y=0 itself (arearenderer.cpp adds
        // its own baseline-closing points and breaks the subpath wherever the
        // curve sits at zero). Adding a lowerSeries instead produces the diagonal-
        // slash corruption, because the upper loop still inserts those zero-breaks
        // but the lower baseline is appended as one run that bridges across them.
        // The area's upperSeries is owned by the area and must NOT also be added to
        // the GraphsView; the crisp coloured stroke stays the separate `line`.
        QColor fc = spec.fillColor.isValid() ? spec.fillColor : spec.color;
        fc.setAlpha(40);
        auto* upper = new QLineSeries(this);
        auto* area = new QAreaSeries(this);
        area->setUpperSeries(upper);   // no lowerSeries: renderer fills to y=0
        area->setColor(fc);
        area->setBorderWidth(0);
        if (setX) area->setAxisX(xax);
        if (setY) area->setAxisY(yax);
        s.area = area;
        s.areaUpper = upper;
    }

    if (setX) line->setAxisX(xax);
    if (setY) line->setAxisY(yax);

    const int id = series_.size();
    series_.append(s);
    // Area behind, line on top (later additions draw over earlier ones).
    if (s.area) addSeriesToView(s.area);
    addSeriesToView(line);
    emit legendChanged();
    return id;
}

void ChartViewModel::addBand(const ChartView::BandSpec& spec)
{
    // No native band item in Qt Graphs — record it; QML draws it as a background
    // rectangle behind the series (mapped through bandRects()).
    bands_.append({ spec.axisId, spec.min, spec.max, spec.color });
    emit bandsChanged();
}

QVariantList ChartViewModel::bandRects() const
{
    QVariantList out;
    for (const Band& b : bands_) {
        if (b.axisId < 0 || b.axisId >= axes_.size()) continue;
        const Axis& a = axes_[b.axisId];
        const double span = a.max - a.min;
        if (span <= 0) continue;
        QVariantMap m;
        m["t0"] = (b.min - a.min) / span;   // bottom (lower value)
        m["t1"] = (b.max - a.min) / span;   // top (higher value)
        m["color"] = b.color;
        out.append(m);
    }
    return out;
}

void ChartViewModel::appendPoint(int seriesId, double x, double y)
{
    if (seriesId < 0 || seriesId >= series_.size()) return;
    series_[seriesId].raw.append(QPointF(x, y));
}

void ChartViewModel::setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys)
{
    if (seriesId < 0 || seriesId >= series_.size()) return;
    QList<QPointF>& raw = series_[seriesId].raw;
    raw.clear();
    const int n = std::min(xs.size(), ys.size());
    raw.reserve(n);
    for (int i = 0; i < n; ++i) raw.append(QPointF(xs[i], ys[i]));
}

void ChartViewModel::trimBefore(int seriesId, double x)
{
    if (seriesId < 0 || seriesId >= series_.size()) return;
    QList<QPointF>& raw = series_[seriesId].raw;
    int cut = 0;
    while (cut < raw.size() && raw[cut].x() < x) ++cut;
    if (cut > 0) raw.remove(0, cut);
}

void ChartViewModel::clear(int seriesId)
{
    if (seriesId < 0 || seriesId >= series_.size()) return;
    series_[seriesId].raw.clear();
}

void ChartViewModel::clearAll()
{
    for (Series& s : series_) s.raw.clear();
}

void ChartViewModel::setSeriesVisible(int seriesId, bool visible)
{
    if (seriesId < 0 || seriesId >= series_.size()) return;
    Series& s = series_[seriesId];
    s.visible = visible;
    if (s.line) s.line->setVisible(visible);
    if (s.area) s.area->setVisible(visible);
    emit legendChanged();
}

void ChartViewModel::setAxisTimeTicker(int axisId)
{
    if (axisId < 0 || axisId >= axes_.size()) return;
    axes_[axisId].fmt = Formatter::Time;
    applyAxisLabelMode();
    buildLabels();
    updateMargins();
}

void ChartViewModel::setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep)
{
    if (axisId < 0 || axisId >= axes_.size()) return;
    Axis& a = axes_[axisId];
    a.fmt = Formatter::Suffix;
    a.scale = scale;
    a.suffix = suffix;
    a.fixedStep = fixedStep;
    applyAxisLabelMode();
    buildLabels();
    updateMargins();
}

void ChartViewModel::setLegendVisible(bool on)
{
    legendVisible_ = on;
    emit legendChanged();
    updateMargins();
}

void ChartViewModel::setHoverReadout(bool on)
{
    hoverEnabled_ = on;
    if (!on) hoverLeave();
}

bool ChartViewModel::seriesKeyRange(int seriesId, double& lo, double& hi) const
{
    if (seriesId < 0 || seriesId >= series_.size()) return false;
    const QList<QPointF>& raw = series_[seriesId].raw;
    if (raw.isEmpty()) return false;
    lo = raw.first().x();
    hi = raw.last().x();
    return true;
}

void ChartViewModel::setXRange(int axisId, double min, double max)
{
    if (axisId < 0 || axisId >= axes_.size()) return;
    Axis& a = axes_[axisId];
    if (a.min == min && a.max == max) return;   // ticks unchanged — skip the rebuild
    a.min = min;
    a.max = max;
    if (a.obj) { a.obj->setMin(min); a.obj->setMax(max); }
    buildLabels();
}

void ChartViewModel::flush(int plotWidthPx)
{
    if (plotWidthPx > 0) lastPlotWidth_ = plotWidthPx;

    // Only decimate for wide time windows (>= 2 min). Narrower windows hold few
    // enough samples to plot in full; the pixel-bucketing reduction kicks in only
    // for the long 2/5/10-min views where the raw point count would otherwise hurt.
    double winSpan = 0.0;
    if (keyAxisId_ >= 0 && keyAxisId_ < axes_.size())
        winSpan = axes_[keyAxisId_].max - axes_[keyAxisId_].min;
    const bool decimate = winSpan >= 120.0;
    // x-extent of one pixel column. Decimators bucket on a fixed absolute grid of
    // this width, so the reduced curve stays put as the window pans (no shimmer).
    const double bucketWidth = winSpan / std::max(1, lastPlotWidth_);

    for (Series& s : series_) {
        if (!s.line) continue;
        // Filled series need a clean single-valued polygon (peakByPixel); plain
        // lines keep the peak-preserving min/max envelope. dropDuplicateX always
        // runs (correctness, not decimation): the session-start burst of samples at
        // t=0 stacks hundreds of points at the same x, which makes the area fill
        // render as a degenerate wedge and tanks tessellation until the window
        // scrolls past it.
        QList<QPointF> dec = decimate
            ? (s.fill ? ChartDecimate::peakByPixel(s.raw, bucketWidth)
                      : ChartDecimate::minMaxByPixel(s.raw, bucketWidth))
            : s.raw;
        dec = ChartDecimate::dropDuplicateX(dec);
        s.line->replace(dec);
        if (s.areaUpper && !dec.isEmpty()) {
            // Nudge exact zeros by an invisible epsilon. The area renderer starts a
            // NEW subpath at every consecutive y==0 pair — that's what gave both the
            // throttle/brake idle lag (hundreds of degenerate subpaths) and the
            // diagonal wedges (subpaths closing to non-baseline points). With no
            // exact zeros the whole curve is ONE subpath the renderer closes to y=0.
            // Sign follows the data; magnitude is ~0.01% of the axis (invisible).
            QList<QPointF> up = dec;
            double peak = 0.0;
            for (const QPointF& p : up)
                if (std::abs(p.y()) > std::abs(peak)) peak = p.y();
            double yspan = 1.0;
            if (s.yAxisId >= 0 && s.yAxisId < axes_.size())
                yspan = std::max(1e-9, axes_[s.yAxisId].max - axes_[s.yAxisId].min);
            const double eps = (peak >= 0.0 ? 1.0 : -1.0) * yspan * 1e-4;
            for (QPointF& p : up)
                if (p.y() == 0.0) p.setY(eps);
            s.areaUpper->replace(up);
        }
    }
    // Ticks depend only on axis range/formatters/theme, not on the data, so they
    // are rebuilt by setXRange/setAxis*Ticker/setThemeColors — not on every flush.
    emit replotRequested();
}

void ChartViewModel::setThemeColors(const QColor& text)
{
    text_ = text;
    buildLabels();
    emit themeChanged();
}

// ── tick label construction ──────────────────────────────────────────────────

QString ChartViewModel::formatTick(const Axis& a, double value) const
{
    switch (a.fmt) {
        case Formatter::Time: {
            const int tenths = int(std::llround(value * 10.0));
            const int whole  = tenths / 10;
            return QString("%1:%2.%3")
                .arg(whole / 60).arg(whole % 60, 2, 10, QChar('0')).arg(std::abs(tenths) % 10);
        }
        case Formatter::Suffix:
            return QString::number(value / a.scale, 'f', a.precision) + a.suffix;
        case Formatter::Plain:
        default:
            return QString::number(value, 'f', a.precision);
    }
}

double ChartViewModel::stepFor(const Axis& a) const
{
    if (a.fmt == Formatter::Suffix && a.fixedStep > 0) return a.fixedStep;
    return niceStep(a.max - a.min);
}

void ChartViewModel::appendTicksFor(const Axis& a)
{
    const double span = a.max - a.min;
    if (span <= 0) return;
    const double step = stepFor(a);
    const QColor c = a.inheritColor ? text_ : a.color;
    const int align = a.isX ? int(Qt::AlignBottom)
                            : int(a.side == ChartView::Side::Left ? Qt::AlignLeft : Qt::AlignRight);

    double first = std::ceil(a.min / step) * step;
    for (double v = first; v <= a.max + step * 1e-6; v += step) {
        QVariantMap m;
        m["isX"] = a.isX;
        m["alignment"] = align;
        m["depth"] = a.sideIndex;             // label column (0 = innermost)
        m["t"] = (v - a.min) / span;          // 0..1 along the axis
        m["text"] = formatTick(a, v);
        m["color"] = c;
        labels_.append(m);
    }
}

// Per-stacked-column spacing for same-side axes. Must match ChartSurface.qml's
// `colW` so the reserved margin lines up with where the overlay draws the labels.
static constexpr double kAxisColW = 40.0;

void ChartViewModel::applyAxisLabelMode()
{
    // Draw printf-friendly, theme-coloured axes natively (Qt-drawn) on every Qt
    // version. On 6.11 a visible axis reserves a fixed ~60px label area; on 6.12
    // GraphsView.dynamicLabelMargins (set in attach()) sizes that area to its
    // content. Qt has no per-axis label colour, so colour-coded axes (Overview's
    // green speed / yellow ERS) and formats Qt can't do (time "m:ss.t", scaled RPM
    // "16k") stay on the overlay. Only the sole axis of a side qualifies — multi-axis
    // sides keep the overlay's column stacking.
    int sideN[3] = {0, 0, 0};   // Bottom, Left, Right
    for (const Axis& a : axes_) sideN[int(a.side)]++;
    for (Axis& a : axes_) {
        const bool friendly = a.fmt == Formatter::Plain
                           || (a.fmt == Formatter::Suffix && a.scale == 1.0);
        a.native = friendly && a.inheritColor && sideN[int(a.side)] == 1;
        if (!a.obj) continue;
        if (a.native) {
            QString f = QString("%.%1f").arg(a.precision);
            if (a.fmt == Formatter::Suffix) {
                QString suf = a.suffix;
                suf.replace("%", "%%");   // escape for the printf-style labelFormat
                f += suf;
            }
            a.obj->setLabelFormat(f);
            a.obj->setLabelsVisible(true);
            a.obj->setVisible(true);
        } else {
            a.obj->setVisible(false);   // overlay draws it; invisible → 0 reserved area
        }
    }
}

void ChartViewModel::updateMargins()
{
    if (!view_) return;

    // Reserve just enough room for our overlay tick labels (Qt Graphs' default
    // margins assume native axis labels + titles, which we hide — that was the
    // wasted space). Estimate each axis's label width from its formatted extremes.
    const double charW = 7.0;   // ~px per char at the overlay's 11px font
    double leftW = 0.0, rightW = 0.0;
    for (const Axis& a : axes_) {
        if (a.isX || a.native) continue;   // native axes are sized by dynamicLabelMargins
        const int chars = std::max(formatTick(a, a.min).size(), formatTick(a, a.max).size());
        const double w = chars * charW + 8.0 + a.sideIndex * kAxisColW;
        if (a.side == ChartView::Side::Left) leftW  = std::max(leftW,  w);
        else                                 rightW = std::max(rightW, w);
    }

    // Floor at 24px each side so the first/last x-axis time label (centred on its
    // tick, ~halfway over the plot edge) has room and doesn't clip.
    view_->setProperty("marginLeft",   std::max(leftW,  24.0));
    view_->setProperty("marginRight",  std::max(rightW, 24.0));
    view_->setProperty("marginBottom", 22.0);                          // x time labels
    view_->setProperty("marginTop",    legendVisible_ ? 24.0 : 8.0);   // overlay legend
}

void ChartViewModel::buildLabels()
{
    labels_.clear();
    for (Axis& a : axes_) {
        // Drive the native axis ticks at the same step as our overlay labels so
        // Qt Graphs' gridlines land exactly under the numbers we draw. Anchored at
        // 0 (and no sub-ticks) so both systems agree on tick positions.
        if (a.obj) {
            const double step = stepFor(a);
            if (step > 0) a.obj->setTickInterval(step);
            a.obj->setTickAnchor(0.0);
            a.obj->setSubTickCount(0);
        }
        if (!a.native) appendTicksFor(a);   // native axes are drawn by Qt
    }
    emit labelsChanged();
}

// ── legend / hover ───────────────────────────────────────────────────────────

QVariantList ChartViewModel::legendEntries() const
{
    QVariantList out;
    for (const Series& s : series_) {
        if (!s.visible || s.name.isEmpty()) continue;
        QVariantMap m;
        m["name"] = s.name;
        m["color"] = s.color;
        out.append(m);
    }
    return out;
}

void ChartViewModel::hoverAt(qreal t)
{
    if (!hoverEnabled_ || keyAxisId_ < 0) { hoverLeave(); return; }
    const Axis& kx = axes_[keyAxisId_];
    const double key = kx.min + t * (kx.max - kx.min);

    const int totalSec = std::max(0, int(key + 0.5));
    QString html = QString("<div style='color:rgba(%1,%2,%3,0.6)'>%4:%5</div>")
        .arg(text_.red()).arg(text_.green()).arg(text_.blue())
        .arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));

    const QLocale loc;
    for (const Series& s : series_) {
        if (!s.visible || s.name.isEmpty() || s.raw.isEmpty()) continue;
        // Nearest sample at or before `key` (raw is sorted by x).
        auto it = std::lower_bound(s.raw.begin(), s.raw.end(), key,
                                   [](const QPointF& p, double k) { return p.x() < k; });
        if (it == s.raw.end()) --it;
        const double value = it->y();
        QString val = s.group ? loc.toString(value, 'f', s.precision)
                              : QString::number(value, 'f', s.precision);
        if (!s.unit.isEmpty()) val += (s.unit == "%" ? "" : " ") + s.unit;
        html += QString("<div style='color:%1'><b>%2:</b> %3</div>")
            .arg(s.color.name(), s.name, val);
    }

    crosshairT_ = t;
    hoverActive_ = true;
    tooltipHtml_ = html;
    emit hoverChanged();
}

void ChartViewModel::hoverLeave()
{
    if (!hoverActive_) return;
    hoverActive_ = false;
    tooltipHtml_.clear();
    emit hoverChanged();
}
