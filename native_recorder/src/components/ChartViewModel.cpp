#include "ChartViewModel.h"
#include "ChartDecimate.h"

#include <QtGraphs/QLineSeries>
#include <QtGraphs/QAreaSeries>
#include <QtGraphs/QValueAxis>
#include <QtGraphs/QDateTimeAxis>
#include <QtGraphs/QAbstractAxis>
#include <QtGraphs/QAbstractSeries>

#include <QQuickItem>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
// QtGraphs' AxisRenderer formats QDateTimeAxis labels with QDateTime::fromMSecsSince-
// Epoch(v).toString(fmt) and NO timezone arg — i.e. always LOCAL time, ignoring the
// axis timeZone. A raw epoch value is therefore shifted by the local UTC offset (e.g.
// +5:30 turns 0s into "30:00"). Pre-subtract that offset so the local-time label
// reads the true elapsed m:ss. Used identically for the axis min/max and series x.
inline qint64 axisMsecFromSec(double sec)
{
    static const qint64 offMs = QDateTime::currentDateTime().offsetFromUtc() * 1000LL;
    return qint64(sec * 1000.0) - offMs;
}
inline QDateTime timeFromSec(double sec)
{
    return QDateTime::fromMSecsSinceEpoch(axisMsecFromSec(sec));
}

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

// Grid + axis lines derive from the palette placeholder-text colour so they read on
// either a light or a dark window. Grid is fainter than the axis baselines.
QColor ChartViewModel::axisLineColor() const { QColor c = line_; c.setAlphaF(0.70); return c; }
QColor ChartViewModel::gridColor()     const { QColor c = line_; c.setAlphaF(0.90); return c; }

void ChartViewModel::attach(QQuickItem* graphsView)
{
    view_ = graphsView;
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    // Size each axis's native label area to its actual content (Qt 6.12+).
    view_->setProperty("dynamicLabelMargins", true);
#endif
    // Any axes/series added before the QML scene was Ready get wired in now.
    for (const Axis& a : axes_) {
        QAbstractAxis* base = a.dateTime ? static_cast<QAbstractAxis*>(a.dtObj)
                                         : static_cast<QAbstractAxis*>(a.obj);
        if (a.isX && !gridXSet_) {
            view_->setProperty("axisX", QVariant::fromValue<QAbstractAxis*>(base));
            gridXSet_ = true;
        }
        if (!a.isX && a.side == ChartView::Side::Left && !gridYSet_) {
            view_->setProperty("axisY", QVariant::fromValue<QAbstractAxis*>(base));
            gridYSet_ = true;
        }
    }
    for (const Series& s : series_) {
        if (s.area) addSeriesToView(s.area);   // fill behind
        addSeriesToView(s.line);               // stroke on top
    }
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
    a.grid = spec.grid;
    a.precision = spec.precision;
    a.dateTime  = spec.dateTime;

    // A dateTime axis renders natively as Qt's QDateTimeAxis ("m:ss"); every other
    // axis is a QValueAxis with a printf labelFormat. Both derive QAbstractAxis.
    QAbstractAxis* base = nullptr;
    if (spec.dateTime) {
        auto* dt = new QDateTimeAxis(this);
        dt->setTimeZone(QTimeZone::utc());
        dt->setMin(timeFromSec(a.min));
        dt->setMax(timeFromSec(a.max));
        dt->setLabelFormat(QStringLiteral("m:ss"));
        dt->setGridVisible(spec.grid);
        dt->setSubGridVisible(false);
        dt->setSubTickCount(0);
        dt->setLabelsVisible(true);
        dt->setVisible(true);
        a.dtObj = dt;
        base = dt;
    } else {
        auto* ax = new QValueAxis(this);
        ax->setMin(spec.min);
        ax->setMax(spec.max);
        ax->setGridVisible(spec.grid);
        ax->setSubGridVisible(false);
        a.obj = ax;
        base = ax;
    }
    switch (spec.side) {
        case ChartView::Side::Bottom: base->setAlignment(Qt::AlignBottom); break;
        case ChartView::Side::Left:   base->setAlignment(Qt::AlignLeft);   break;
        case ChartView::Side::Right:  base->setAlignment(Qt::AlignRight);  break;
    }

    const int id = axes_.size();
    axes_.append(a);
    applyAxisFormat(axes_[id]);
    if (a.isX && keyAxisId_ < 0) keyAxisId_ = id;
    if (spec.side == ChartView::Side::Left && primaryLeftId_ < 0) primaryLeftId_ = id;

    // The GraphsView needs a primary x/y axis to define the plot area + grid.
    if (view_) {
        if (a.isX && !gridXSet_) {
            view_->setProperty("axisX", QVariant::fromValue<QAbstractAxis*>(base));
            gridXSet_ = true;
        }
        if (!a.isX && spec.side == ChartView::Side::Left && !gridYSet_) {
            view_->setProperty("axisY", QVariant::fromValue<QAbstractAxis*>(base));
            gridYSet_ = true;
        }
    }
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
    s.yScale = spec.yScale;
    s.fill = spec.fill;

    auto* line = new QLineSeries(this);
    line->setName(spec.name);
    line->setColor(spec.color);
    line->setWidth(spec.width);
    if (spec.step) line->setLineStyle(QLineSeries::LineStyle::StepLeft);
    s.line = line;

    auto baseAxis = [this](int id) -> QAbstractAxis* {
        if (id < 0 || id >= axes_.size()) return nullptr;
        const Axis& a = axes_[id];
        return a.dateTime ? static_cast<QAbstractAxis*>(a.dtObj)
                          : static_cast<QAbstractAxis*>(a.obj);
    };
    QAbstractAxis* xax = baseAxis(spec.xAxisId);
    QAbstractAxis* yax = baseAxis(spec.yAxisId);
    // Axis attachment rule: only attach an axis EXPLICITLY when it is a secondary
    // (non-primary) axis. The GraphsView's primary x/y axes must be INHERITED, never
    // re-passed per series — re-associating the primary X to a series (as the
    // secondary-Y RPM line does) makes Qt Graphs render that X axis a second time
    // with its own independently auto-computed ticks, so its grid drifts away from
    // the primary axis' labels. Inheriting keeps one set of X ticks for everyone.
    const bool setX = xax && spec.xAxisId != keyAxisId_;
    const bool setY = yax && spec.yAxisId != primaryLeftId_;

    if (spec.fill) {
        // Translucent fill with ONLY an upperSeries and NO lowerSeries: the area
        // renderer then fills the curve down to y=0 itself.
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

void ChartViewModel::setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep)
{
    if (axisId < 0 || axisId >= axes_.size()) return;
    Axis& a = axes_[axisId];
    a.scale = scale;
    a.suffix = suffix;
    a.fixedStep = fixedStep;
    applyAxisFormat(a);
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
    if (a.min == min && a.max == max) return;
    a.min = min;
    a.max = max;
    if (a.dateTime && a.dtObj) { a.dtObj->setMin(timeFromSec(min)); a.dtObj->setMax(timeFromSec(max)); }
    else if (a.obj)           { a.obj->setMin(min);                a.obj->setMax(max); }
    applyAxisFormat(a);   // re-pick the nice tick interval for the new span
}

void ChartViewModel::flush(int plotWidthPx)
{
    if (plotWidthPx > 0) lastPlotWidth_ = plotWidthPx;

    // Only decimate for wide time windows (>= 2 min).
    double winSpan = 0.0;
    if (keyAxisId_ >= 0 && keyAxisId_ < axes_.size())
        winSpan = axes_[keyAxisId_].max - axes_[keyAxisId_].min;
    const bool decimate = winSpan >= 120.0;
    const double bucketWidth = winSpan / std::max(1, lastPlotWidth_);

    for (Series& s : series_) {
        if (!s.line) continue;
        QList<QPointF> dec = decimate
            ? (s.fill ? ChartDecimate::peakByPixel(s.raw, bucketWidth)
                      : ChartDecimate::minMaxByPixel(s.raw, bucketWidth))
            : s.raw;
        dec = ChartDecimate::dropDuplicateX(dec);
        // Convert to plot units now: a dateTime x axis wants msecs-since-epoch, and
        // a scaled axis (e.g. RPM 0–16) wants y × yScale. The raw store is untouched.
        const bool xMsec = (s.xAxisId >= 0 && s.xAxisId < axes_.size() && axes_[s.xAxisId].dateTime);
        if (xMsec || s.yScale != 1.0) {
            for (QPointF& p : dec) {
                if (xMsec)           p.setX(double(axisMsecFromSec(p.x())));
                if (s.yScale != 1.0) p.setY(p.y() * s.yScale);
            }
        }
        s.line->replace(dec);
        if (s.areaUpper && !dec.isEmpty()) {
            // Nudge exact zeros by an invisible epsilon so the area is ONE subpath.
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
    emit replotRequested();
}

void ChartViewModel::setThemeColors(const QColor& text, const QColor& line)
{
    text_ = text;
    line_ = line;
    emit themeChanged();
}

// ── native axis formatting ─────────────────────────────────────────────────────

QString ChartViewModel::labelFormatFor(const Axis& a) const
{
    // printf-style label format for QValueAxis (e.g. "%.0f", "%.1f G", "%.0fk").
    QString f = QStringLiteral("%.") + QString::number(a.precision) + QLatin1Char('f');
    if (!a.suffix.isEmpty()) {
        QString suf = a.suffix;
        suf.replace(QLatin1Char('%'), QStringLiteral("%%"));   // escape for printf
        f += suf;
    }
    return f;
}

void ChartViewModel::applyAxisFormat(Axis& a)
{
    if (a.dateTime) {
        // Let QDateTimeAxis auto-compute its major ticks (labels + grid); just
        // suppress sub-ticks. (An explicit tickInterval here is interpreted in a
        // unit that blew the tick count up, so it's left auto.)
        if (a.dtObj) a.dtObj->setSubTickCount(0);
        return;
    }
    if (!a.obj) return;
    a.obj->setLabelFormat(labelFormatFor(a));
    a.obj->setLabelsVisible(true);
    a.obj->setVisible(true);
    a.obj->setSubTickCount(0);
    a.obj->setTickAnchor(0.0);
    const double step = (a.fixedStep > 0.0) ? a.fixedStep : niceStep(a.max - a.min);
    if (step > 0.0) a.obj->setTickInterval(step);
}

void ChartViewModel::updateMargins()
{
    if (!view_) return;

    // Native axis labels are drawn by Qt in the margin area, so each side's margin
    // must be wide enough for its labels. Estimate from the formatted extremes.
    const double charW = 7.0;   // ~px per char at the 11px label font
    auto labelW = [&](const Axis& a) -> double {
        if (a.dateTime) return 5 * charW;   // "m:ss"
        const QString fmt = labelFormatFor(a);
        const QString lo = QString::asprintf(fmt.toUtf8().constData(), a.min);
        const QString hi = QString::asprintf(fmt.toUtf8().constData(), a.max);
        return std::max(lo.size(), hi.size()) * charW;
    };

    double leftW = 0.0, rightW = 0.0;
    bool haveLeft = false, haveRight = false, haveBottom = false;
    for (const Axis& a : axes_) {
        if (a.isX) { haveBottom = true; continue; }
        if (a.side == ChartView::Side::Left)  { leftW  = std::max(leftW,  labelW(a)); haveLeft  = true; }
        else                                  { rightW = std::max(rightW, labelW(a)); haveRight = true; }
    }

    view_->setProperty("marginLeft",   haveLeft  ? std::max(leftW  + 12.0, 24.0) : 8.0);
    view_->setProperty("marginRight",  haveRight ? std::max(rightW + 12.0, 24.0) : 8.0);
    view_->setProperty("marginBottom", haveBottom ? 28.0 : 8.0);
    view_->setProperty("marginTop",    legendVisible_ ? 24.0 : 8.0);   // overlay legend
}

// ── legend / hover (kept custom — no native Qt Graphs equivalent) ───────────────

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
