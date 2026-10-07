#include "TrendChart.h"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {
// Diameter of the dot marking each lap's value on a per-lap line.
constexpr double kLapDotSize = 8;
constexpr double kBarGroupWidth = 0.8;
constexpr double kBarGapFraction = 0.15;
// Corners of one bar share an x; keep them in increasing order.
constexpr double kBarEdge = 1e-4;
// Electron TimeChartView's default series width.
constexpr double kLineWidth = 1.5;
constexpr double kNiceSteps[] = {1, 2, 2.5, 5};

double niceNum(double range, bool round) {
    const double exponent = std::floor(std::log10(range));
    const double fraction = range / std::pow(10.0, exponent);
    double nice;
    if (round) nice = fraction < 1.5 ? 1 : fraction < 3 ? 2 : fraction < 7 ? 5 : 10;
    else nice = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
    return nice * std::pow(10.0, exponent);
}

// Electron's niceTicks: round values only, within [min, max].
QVector<double> niceTicks(double min, double max, int count = 5) {
    QVector<double> ticks;
    if (!std::isfinite(min) || !std::isfinite(max)) return ticks;
    if (min == max) return {min};
    const double range = niceNum(max - min, false);
    const double step = niceNum(range / std::max(1, count - 1), true);
    const double niceMin = std::ceil(min / step) * step;
    const double niceMax = std::floor(max / step) * step;
    for (int i = 0; i < 100; ++i) {
        const double value = niceMin + i * step;
        if (value > niceMax + step * 0.5) break;
        ticks.push_back(std::clamp(std::round(value * 1e6) / 1e6, min, max));
    }
    return ticks;
}

QString lapX(double value) { return QStringLiteral("L%1").arg(qRound(value)); }
QString missingValue() { return QString::fromUtf8("—"); }
} // namespace

TrendRange quarterAxisRange(double lo, double hi, double floor) {
    const bool pinned = std::isfinite(floor);
    if (!std::isfinite(lo) || !std::isfinite(hi))
        return {pinned ? floor : 0.0, (pinned ? floor : 0.0) + 1.0};
    if (pinned) lo = floor;
    const double span = std::max(hi - lo, 1e-6);
    const double magnitude = std::pow(10.0, std::floor(std::log10(span / 4)));
    for (double scale : {1.0, 10.0, 100.0}) for (double nice : kNiceSteps) {
        const double step = nice * magnitude * scale;
        const double min = pinned ? floor : std::floor(lo / step) * step;
        if (min + 4 * step >= hi) return {min, min + 4 * step};
    }
    return {lo, hi};
}

TrendChart::TrendChart(const Config& config, QWidget* parent)
    : ChartView(parent), config_(config) {
    // Electron's tyre-chart look: dashed grid on both axes and 3 px x ticks.
    xId_ = addAxis({ Side::Bottom, 0.0, 1.0, QColor(), true, 'f', 0, true, 60 });
    setAxisGridStyle(xId_, true, 3);
    const int scales = std::max(1, config_.axisCount);
    for (int i = 0; i < scales; ++i) {
        const int id = addAxis({ i == 0 ? Side::Left : Side::Right, 0.0, 1.0, QColor(), true, 'f', 0, i == 0 });
        if (i == 0) setAxisGridStyle(id, true);
        else setAxisSide(id, Side::Right, i - 1);   // the first extra scale sits nearest the plot
        yIds_.push_back(id);
    }
    // Build the header (title + colour key) before the series so they join its legend.
    setPanelTitle(0, config_.title.toUpper());
    setPanelLegendVisible(0, true);

    for (int i = 0; i < config_.series.size(); ++i) {
        const TrendSeries& item = config_.series[i];
        const int yAxis = yIds_[config_.axisCount > 0 ? std::clamp(item.axis, 0, scales - 1) : 0];
        SeriesSpec spec;
        spec.name = item.label;
        spec.color = item.color;
        spec.width = kLineWidth;
        spec.xAxisId = xId_;
        spec.yAxisId = yAxis;
        if (config_.bars) {
            // A bar is read from its tooltip; a marker on a corner would mislead.
            spec.fill = true;
            spec.fillColor = item.color;
            spec.hoverSnap = HoverSnap::None;
        } else if (item.perLap) {
            // Hovering inside lap N marks the value closing it.
            spec.hoverSnap = HoverSnap::Next;
        }
        lineIds_.push_back(addSeries(spec));
        int dots = -1;
        if (item.perLap && !config_.bars) {
            // Per-lap lines carry a dot at each lap's value, drawn above the line.
            SeriesSpec dot;
            dot.color = item.color;
            dot.width = kLapDotSize;
            dot.xAxisId = xId_;
            dot.yAxisId = yAxis;
            dot.lineType = LineType::RoundPoint;
            dot.hoverSnap = HoverSnap::None;
            dots = addSeries(dot);
            linkSeriesVisibility(lineIds_.last(), dots);
        }
        dotIds_.push_back(dots);
    }
    // Reversed so the first listed is drawn last, on top; a stack's bottom
    // segment, listed first, is its shortest rectangle and so stays visible.
    // Per-lap lines are drawn above the rest.
    QVector<int> order;
    for (int i = config_.series.size() - 1; i >= 0; --i)
        if (!config_.series[i].perLap) order.push_back(lineIds_[i]);
    for (int i = config_.series.size() - 1; i >= 0; --i) {
        if (!config_.series[i].perLap) continue;
        order.push_back(lineIds_[i]);
        if (dotIds_[i] >= 0) order.push_back(dotIds_[i]);
    }
    setSeriesOrder(order);

    setHoverReadout(true);
    setCursorSync(false, true, false);
    if (config_.syncOrder >= 0) setPanelSyncOrder(0, config_.syncOrder);
    setData({}, {}, {});
}

void TrendChart::setTitle(const QString& title) {
    if (config_.title == title) return;
    config_.title = title;
    setPanelTitle(0, title.toUpper());
}

void TrendChart::setSeriesColors(const QVector<QColor>& colors) {
    for (int i = 0; i < config_.series.size() && i < colors.size(); ++i) {
        config_.series[i].color = colors[i];
        setSeriesColor(lineIds_[i], colors[i]);
        if (config_.bars) setSeriesFillColor(lineIds_[i], colors[i]);
        if (dotIds_[i] >= 0) setSeriesColor(dotIds_[i], colors[i]);
    }
    requestReplot();
}

void TrendChart::applyCursorSync(bool enabled, bool secondaryVertical, bool secondaryHorizontal) {
    setCursorSync(config_.syncOrder >= 0 && enabled, secondaryVertical, secondaryHorizontal);
}

// Each series' value channel: its position within its own group by default.
int TrendChart::channelOf(int index) const {
    const TrendSeries& item = config_.series[index];
    if (item.channel >= 0) return item.channel;
    int position = 0;
    for (int i = 0; i < index; ++i)
        if (config_.series[i].perLap == item.perLap) ++position;
    return position;
}

void TrendChart::setData(const QVector<TrendPoint>& points, const QVector<TrendPoint>& lapPoints,
                         const QVector<TrendAxis>& axes) {
    points_ = points;
    lapPoints_ = lapPoints;
    axes_ = axes;

    // X: one tick per lap, padded half a lap for discrete per-lap values.
    double first = std::numeric_limits<double>::infinity();
    double last = -std::numeric_limits<double>::infinity();
    if (!points_.isEmpty()) { first = points_.first().x; last = points_.last().x; }
    if (!lapPoints_.isEmpty()) {
        first = std::min(first, lapPoints_.first().x);
        last = std::max(last, lapPoints_.last().x);
    }
    double lo = 0, hi = 1;
    if (std::isfinite(first)) {
        lo = config_.discrete ? first - 0.5 : first;
        hi = config_.discrete ? last + 0.5 : std::max(last, first + 1);
    }
    QVector<double> ticks;
    QStringList labels;
    for (int lap = int(std::ceil(lo)); lap <= int(std::floor(hi)); ++lap) {
        ticks.push_back(lap);
        labels.push_back(lapX(lap));
    }
    setAxisLabelMap(xId_, ticks, labels);
    setXRange(xId_, lo, hi);

    // Laps map to session time through their own points for the shared tooltip.
    QVector<double> laps, starts;
    for (const TrendPoint& point : points_)
        if (std::isfinite(point.lapStart)) { laps.push_back(point.x); starts.push_back(point.lapStart); }
    setAxisLapMap(xId_, laps, starts);

    rowXs_.clear();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto valueOf = [nan](const TrendPoint& point, int channel) {
        return channel >= 0 && channel < point.values.size() ? point.values[channel] : nan;
    };
    if (config_.bars) {
        // Bar outlines as line rows: for each point, one bar per slot, side by
        // side across kBarGroupWidth of a lap. A slot of several series is a
        // stack: each series' rectangle reaches the running total up to it.
        QVector<QVector<int>> barSlots;
        QHash<QString, int> stackSlot;
        QVector<int> slotOf(config_.series.size(), -1);
        for (int i = 0; i < config_.series.size(); ++i) {
            const TrendSeries& item = config_.series[i];
            if (item.perLap) continue;
            if (!item.stack.isEmpty() && stackSlot.contains(item.stack)) {
                slotOf[i] = stackSlot.value(item.stack);
                barSlots[slotOf[i]].push_back(i);
                continue;
            }
            slotOf[i] = int(barSlots.size());
            barSlots.push_back({i});
            if (!item.stack.isEmpty()) stackSlot.insert(item.stack, slotOf[i]);
        }
        const double width = kBarGroupWidth / std::max<qsizetype>(barSlots.size(), 1);
        const double gap = width * kBarGapFraction / 2;
        for (const TrendPoint& point : points_)
            for (int k = 0; k < barSlots.size(); ++k) {
                const double left = point.x - kBarGroupWidth / 2 + k * width + gap;
                const double right = left + width - 2 * gap;
                rowXs_ << left << left + kBarEdge << right - kBarEdge << right;
            }
        std::sort(rowXs_.begin(), rowXs_.end());
        for (int i = 0; i < config_.series.size(); ++i) {
            if (slotOf[i] < 0) { setSeriesData(lineIds_[i], {}, {}); continue; }
            const int k = slotOf[i];
            QVector<double> xs, ys;
            xs.reserve(points_.size() * 4);
            ys.reserve(points_.size() * 4);
            for (const TrendPoint& point : points_) {
                const double left = point.x - kBarGroupWidth / 2 + k * width + gap;
                const double right = left + width - 2 * gap;
                double total = 0;
                for (int member : barSlots[k]) {
                    const double value = valueOf(point, channelOf(member));
                    total += std::isfinite(value) ? value : 0;
                    if (member == i) break;
                }
                xs << left << left + kBarEdge << right - kBarEdge << right;
                ys << 0.0 << total << total << 0.0;
            }
            setSeriesData(lineIds_[i], xs, ys);
        }
    } else {
        for (const TrendPoint& point : points_) rowXs_.push_back(point.x);
        for (int i = 0; i < config_.series.size(); ++i) {
            const QVector<TrendPoint>& source = config_.series[i].perLap ? lapPoints_ : points_;
            const int channel = channelOf(i);
            QVector<double> xs, ys;
            xs.reserve(source.size());
            ys.reserve(source.size());
            for (const TrendPoint& point : source) {
                xs.push_back(point.x);
                ys.push_back(valueOf(point, channel));
            }
            setSeriesData(lineIds_[i], xs, ys);
            if (dotIds_[i] >= 0) setSeriesData(dotIds_[i], xs, ys);
        }
    }
    updateYAxes();
    requestReplot();
}

void TrendChart::updateYAxes() {
    const auto applyTicks = [this](int axisId, const QVector<double>& ticks, const TrendFormat& format) {
        QStringList labels;
        for (double tick : ticks) labels.push_back(format ? format(tick) : QString::number(tick));
        setAxisLabelMap(axisId, ticks, labels);
    };
    if (config_.axisCount > 0) {
        // Every axis labels the same quarter lines in its own units, so all
        // labels sit on the grid.
        for (int i = 0; i < config_.axisCount && i < yIds_.size(); ++i) {
            const TrendAxis axis = i < axes_.size() ? axes_[i] : TrendAxis{};
            const double min = axis.min, max = axis.max > axis.min ? axis.max : axis.min + 1;
            const QVector<double> ticks{min, min + 0.25 * (max - min), min + 0.5 * (max - min),
                                        min + 0.75 * (max - min), max};
            applyTicks(yIds_[i], ticks, axis.format);
            setAxisRange(yIds_[i], min, max);
            if (axis.color.isValid()) setAxisColor(yIds_[i], axis.color);
        }
        return;
    }
    // Always derived from the points: auto ranging a single lap has no height
    // and flips between a degenerate and a padded axis.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (const TrendPoint& point : points_)
        for (double value : point.values)
            if (std::isfinite(value)) { lo = std::min(lo, value); hi = std::max(hi, value); }
    double min = 0, max = 1;
    if (std::isfinite(config_.zeroBaselineHeadroom)) {
        min = 0;
        max = std::max(hi, 0.0) + config_.zeroBaselineHeadroom;
    } else if (std::isfinite(lo)) {
        const double pad = hi > lo ? (hi - lo) * 0.1 : 1;
        min = lo - pad;
        max = hi + pad;
    }
    // Round values only: a range end such as max + 0.5 MJ is not worth a label.
    applyTicks(yIds_[0], niceTicks(min, max, 5), config_.formatY);
    setAxisRange(yIds_[0], min, max);
}

double TrendChart::snappedRowX(double key) const {
    if (rowXs_.isEmpty()) return key;
    const auto it = std::lower_bound(rowXs_.cbegin(), rowXs_.cend(), key);
    if (it == rowXs_.cbegin()) return *it;
    if (it == rowXs_.cend()) return rowXs_.last();
    const double after = *it, before = *(it - 1);
    return key - before <= after - key ? before : after;
}

int TrendChart::nearestPoint(double x) const {
    if (points_.isEmpty()) return -1;
    const auto it = std::lower_bound(points_.cbegin(), points_.cend(), x,
        [](const TrendPoint& point, double value) { return point.x < value; });
    int index = std::min(int(std::distance(points_.cbegin(), it)), int(points_.size()) - 1);
    if (index > 0 && std::abs(points_[index - 1].x - x) < std::abs(points_[index].x - x)) --index;
    return index;
}

QString TrendChart::pointLabel(const TrendPoint& point) const {
    return point.label + (point.invalid ? QStringLiteral(" · Invalid") : QString())
                       + (point.fastest ? QStringLiteral(" · Fastest") : QString())
                       + (point.provisional ? QStringLiteral(" · In progress") : QString());
}

TrendFormat TrendChart::tooltipFormatFor(int seriesIndex) const {
    if (config_.axisCount > 0) {
        const int index = std::clamp(config_.series[seriesIndex].axis, 0, config_.axisCount - 1);
        if (index < axes_.size()) {
            const TrendAxis& axis = axes_[index];
            return axis.formatTooltip ? axis.formatTooltip : axis.format;
        }
    }
    return config_.formatTooltipY ? config_.formatTooltipY : config_.formatY;
}

// The visible series' rows; per-lap series read `lap`, the rest `main`.
ChartView::TooltipContent TrendChart::valueLines(const TrendPoint* main, const TrendPoint* lap) const {
    TooltipContent lines;
    for (int i = 0; i < config_.series.size(); ++i) {
        if (!seriesVisible(lineIds_[i])) continue;
        const TrendSeries& item = config_.series[i];
        const TrendPoint* point = item.perLap ? lap : main;
        const int channel = channelOf(i);
        const double value = point && channel < point->values.size()
            ? point->values[channel] : std::numeric_limits<double>::quiet_NaN();
        const TrendFormat format = tooltipFormatFor(i);
        const QString text = std::isfinite(value) ? (format ? format(value) : QString::number(value))
                                                  : missingValue();
        lines << tooltipValueLine(item.label, item.color, text);
    }
    return lines;
}

bool TrendChart::hoverSample(int, double key, double& sampled) const {
    // Like Electron, the tooltip follows the pointer even where no row is.
    sampled = snappedRowX(key);
    return true;
}

bool TrendChart::customTooltip(int, double key, TooltipContent& out) const {
    const double x = snappedRowX(key);
    const int index = nearestPoint(x);
    const TrendPoint* point = index >= 0 ? &points_[index] : nullptr;
    const auto header = [this](const QString& text) {
        TooltipLine line = tooltipTextLine(text, tooltipMutedColor());
        line.marginBottom = 3;
        return line;
    };
    const bool perLap = std::any_of(config_.series.cbegin(), config_.series.cend(),
                                    [](const TrendSeries& item) { return item.perLap; });
    if (perLap) {
        // The hovered lap's values are those at its closing line.
        const int lap = point && point->lap > 0 ? point->lap : int(std::floor(x)) + 1;
        const auto closing = std::find_if(lapPoints_.cbegin(), lapPoints_.cend(),
                                          [lap](const TrendPoint& candidate) { return candidate.x == lap; });
        const TrendPoint* closingPoint = closing != lapPoints_.cend() ? &*closing : nullptr;
        out << header(closingPoint ? pointLabel(*closingPoint)
                                   : QStringLiteral("Lap %1 · In progress").arg(lap));
        out += valueLines(point, closingPoint);
        return true;
    }
    // The point holds the raw values; the bars' own are running totals.
    out << header(point ? pointLabel(*point) : lapX(x));
    if (point) out += valueLines(point, nullptr);
    return true;
}

// Electron formatRow: "<title> · <lap>" then the lap's values, for the lap
// nearest the synchronized x; nothing before this chart's first lap.
bool TrendChart::customSyncedRows(int, double key, TooltipContent& out) const {
    out.clear();
    const int index = nearestPoint(key);
    if (index < 0 || key < points_.first().x - (config_.discrete ? 0.5 : 0.0)) return true;
    const TrendPoint& point = points_[index];
    TooltipLine title = tooltipTextLine(config_.title + QStringLiteral(" · ") + pointLabel(point),
                                        tooltipMutedColor());
    title.marginTop = 3;
    out << title;
    out += valueLines(&point, nullptr);
    return true;
}
