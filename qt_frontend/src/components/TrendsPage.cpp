#include "TrendsPage.h"
#include "CardColors.h"
#include "PageUiHelpers.h"
#include "SummaryCard.h"
#include "TrendChart.h"
#include "TrendTyreChart.h"
#include "TyreHelpers.h"
#include "../Labels.h"
#include "../PlaybackPatchMerger.h"
#include "../PresentationScheduler.h"
#include "../SessionModel.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLayout>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace {

using Lap = tnrp::SessionHistoryLap;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Electron fmtMs.
QString fmtMs(qint64 ms) {
    if (ms <= 0) return QStringLiteral("--:--.---");
    return QStringLiteral("%1:%2.%3").arg(ms / 60000)
        .arg((ms % 60000) / 1000, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}
// JavaScript's +value.toFixed(decimals): no trailing zeros.
QString shortNumber(double value, int decimals) {
    QString text = QString::number(value, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    }
    return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}
// Axis labels must fit the tyre-chart gutter; tooltips keep full precision.
QString lapAxisY(double value) {
    const qint64 tenths = qRound64(value * 10);
    return QStringLiteral("%1:%2").arg(tenths / 600)
        .arg(QString::number((tenths % 600) / 10.0, 'f', 1).rightJustified(4, QLatin1Char('0')));
}
QString lapTooltipY(double value) { return fmtMs(qRound64(value * 1000)); }
QString energyAxisY(double value) { return QString::number(value, 'f', 1) + QStringLiteral(" MJ"); }
QString energyTooltipY(double value) { return QString::number(value, 'f', 2) + QStringLiteral(" MJ"); }
QString energyAxisLabel(double value) { return shortNumber(value, 2) + QStringLiteral(" MJ"); }
QString percentAxisLabel(double value) { return shortNumber(value, 1) + QStringLiteral("%"); }
QString percentTooltip(double value) { return QString::number(value, 'f', 1) + QStringLiteral("%"); }

// Kept apart from each other and from the four tyre corner colours, which
// share the page and, in the combined layout, the plot.
struct TrendColors { QColor lapTime, used, recharged; };
TrendColors trendColors() {
    return tnr::isDarkTheme()
        ? TrendColors{QColor("#B0E0FF"), QColor("#FF9830"), QColor("#FF6EC7")}
        : TrendColors{QColor("#004D61"), QColor("#4A148C"), QColor("#880E4F")};
}
QColor cornerColor(int corner) {
    switch (corner) {
        case 0:  return QColor("#e10600");
        case 1:  return tnr::themed("#4488ff", "#0B57D0");
        case 2:  return tnr::themed("#37872D", "#137333");
        default: return tnr::themed("#ffd700", "#765900");
    }
}
const char* const kCorners[4] = {"FL", "FR", "RL", "RR"};

QString combinedTitle(bool life) {
    return life ? QStringLiteral("Lap Times · ERS · Tyre Life") : QStringLiteral("Lap Times · ERS · Tyre Wear");
}
QString barsTitle(bool life) {
    return life ? QStringLiteral("Tyre Life · ERS · Lap Times") : QStringLiteral("Tyre Wear · ERS · Lap Times");
}

// Mean of the measured values, with how many completed laps it covers.
struct Average { QString value, title, coverage; };
Average average(const QVector<double>& values) {
    double sum = 0;
    int measured = 0;
    for (double value : values) if (std::isfinite(value)) { sum += value; ++measured; }
    if (measured == 0) return {SummaryCard::kMissing, QString(), QString()};
    return {
        QString::number(sum / measured, 'f', 2),
        measured < values.size()
            ? QStringLiteral("Measured on %1 of %2 completed laps").arg(measured).arg(values.size()) : QString(),
        QStringLiteral("%1 of %2 laps measured").arg(measured).arg(values.size()),
    };
}

std::optional<int> fastestValid(const QVector<Lap>& laps) {
    std::optional<int> best;
    for (const Lap& lap : laps)
        if (lap.lap_valid && lap.lap_time_ms > 0 && (!best || lap.lap_time_ms < *best)) best = lap.lap_time_ms;
    return best;
}

double lapSeconds(const Lap& lap) { return lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000.0 : kNaN; }

double wearValue(float wear, bool life) {
    return std::isfinite(wear) ? (life ? 100.0 - wear : double(wear)) : kNaN;
}

// Lap start times in one pass; a reused lap number keeps its latest attempt.
QHash<int, double> lapStarts(const QVector<TrendLapBoundary>& boundaries) {
    QHash<int, double> starts;
    for (const TrendLapBoundary& boundary : boundaries) starts.insert(boundary.lapNum, boundary.sessionTime);
    return starts;
}

// Index of the last damage row before `time` (-1 when none).
qsizetype lastBefore(const QVector<DamageSample>& rows, double time) {
    const auto it = std::lower_bound(rows.cbegin(), rows.cend(), time,
        [](const DamageSample& sample, double value) { return sample.t < value; });
    return qsizetype(std::distance(rows.cbegin(), it)) - 1;
}

// ERS used or recharged per completed lap; it gains a point only when a lap
// completes, like lap time.
QVector<TrendPoint> energyPoints(bool deployed, const QVector<Lap>& laps,
                                 const QHash<int, TrendLapMeasure>& measures,
                                 const QVector<TrendLapBoundary>& boundaries) {
    QVector<TrendPoint> points;
    for (const Lap& lap : laps) {
        const TrendLapMeasure measure = measures.value(lap.lap_num);
        TrendPoint point;
        point.x = lap.lap_num;
        point.values = {deployed ? measure.deployedMj : measure.harvestedMj};
        point.label = QStringLiteral("Lap %1").arg(lap.lap_num);
        point.lapStart = trendLapStart(boundaries, lap.lap_num);
        points.push_back(point);
    }
    return points;
}

// The combined graph's per-lap values, one point per completed lap at x = N,
// the line closing lap N: ERS used, ERS recharged and lap time. Nothing is
// plotted for a lap until it is complete.
QVector<TrendPoint> buildLapEndPoints(const QVector<Lap>& laps, const QHash<int, TrendLapMeasure>& measures) {
    const std::optional<int> fastest = fastestValid(laps);
    QVector<TrendPoint> points;
    for (const Lap& lap : laps) {
        const TrendLapMeasure measure = measures.value(lap.lap_num);
        TrendPoint point;
        point.x = lap.lap_num;
        point.label = QStringLiteral("Lap %1").arg(lap.lap_num);
        point.values = {measure.deployedMj, measure.harvestedMj, lapSeconds(lap)};
        point.invalid = lap.lap_time_ms > 0 && !lap.lap_valid;
        point.fastest = lap.lap_valid && fastest && lap.lap_time_ms == *fastest;
        points.push_back(point);
    }
    return points;
}

// Tyre samples kept per lap: the line stays live without carrying every
// damage packet of the session into the chart.
constexpr int kTyreSamplesPerLap = 40;
constexpr double kFallbackLapS = 90;

// The combined graph's live tyre wear (or life): a sample taken partway
// through lap N sits at N - 1 plus the fraction of the lap elapsed, so lap
// N's closing line is at x = N. The lap in progress is spaced by the
// previous lap's time.
QVector<TrendPoint> buildTyreSamples(const QVector<Lap>& laps, int firstLap, std::optional<int> currentLap,
                                     const QVector<TrendLapBoundary>& boundaries,
                                     const QVector<DamageSample>& rows, bool life) {
    QVector<TrendPoint> points;
    if (rows.isEmpty() || !currentLap) return points;
    const QHash<int, double> starts = lapStarts(boundaries);
    const int lastLapTime = laps.isEmpty() ? 0 : laps.last().lap_time_ms;
    const double estimatedLapS = lastLapTime > 0 ? lastLapTime / 1000.0 : kFallbackLapS;
    qsizetype previousIndex = -1;
    for (int lap = firstLap; lap <= *currentLap; ++lap) {
        if (!starts.contains(lap)) continue;
        const double start = starts.value(lap);
        const bool ended = starts.contains(lap + 1);
        const double end = ended ? starts.value(lap + 1) : kNaN;
        const double duration = ended ? end - start : estimatedLapS;
        // The newest sample in each slice of the lap stands for it; a binary
        // search finds it, so the cost does not grow with the packet rate.
        for (int slice = 1; slice <= kTyreSamplesPerLap; ++slice) {
            // The lap in progress may run longer than estimated: its last slice is open.
            const double sliceEnd = slice == kTyreSamplesPerLap
                ? (ended ? end : std::numeric_limits<double>::infinity())
                : start + duration * slice / kTyreSamplesPerLap;
            const qsizetype index = lastBefore(rows, sliceEnd);
            if (index <= previousIndex || index < 0) continue;
            const double time = rows[index].t;
            if (time < start) continue;
            previousIndex = index;
            const DamageSample& sample = rows[index];
            TrendPoint point;
            point.x = lap - 1 + std::min((time - start) / duration, 0.999);
            point.label = QStringLiteral("Lap %1").arg(lap);
            point.lap = lap;
            point.values = {wearValue(sample.wearFl, life), wearValue(sample.wearFr, life),
                            wearValue(sample.wearRl, life), wearValue(sample.wearRr, life)};
            points.push_back(point);
        }
    }
    return points;
}

// The bar graph's points, one per completed lap at x = lap number: each
// corner's tyre wear (or life) at the lap's closing line, ERS used, and lap time.
QVector<TrendPoint> buildBarPoints(const QVector<Lap>& laps, const QHash<int, TrendLapMeasure>& measures,
                                   const QVector<TrendLapBoundary>& boundaries,
                                   const QVector<DamageSample>& rows, bool life) {
    const QHash<int, double> starts = lapStarts(boundaries);
    const std::optional<int> fastest = fastestValid(laps);
    QVector<TrendPoint> points;
    for (const Lap& lap : laps) {
        const qsizetype index = starts.contains(lap.lap_num + 1)
            ? lastBefore(rows, starts.value(lap.lap_num + 1)) : -1;
        TrendPoint point;
        point.x = lap.lap_num;
        point.label = QStringLiteral("Lap %1").arg(lap.lap_num);
        if (index >= 0) {
            const DamageSample& sample = rows[index];
            point.values = {wearValue(sample.wearFl, life), wearValue(sample.wearFr, life),
                            wearValue(sample.wearRl, life), wearValue(sample.wearRr, life)};
        } else {
            point.values = {kNaN, kNaN, kNaN, kNaN};
        }
        point.values << measures.value(lap.lap_num).deployedMj << lapSeconds(lap);
        point.invalid = lap.lap_time_ms > 0 && !lap.lap_valid;
        point.fastest = lap.lap_valid && fastest && lap.lap_time_ms == *fastest;
        points.push_back(point);
    }
    return points;
}

// The bar graph's scales: lap time on the left, then MJ and tyre %.
QVector<TrendAxis> barAxes(const QVector<TrendPoint>& points, const QColor& lapColor, bool dynamicTyres) {
    // The tyre scale fits the tallest stack: the four corners together.
    const auto highest = [&points](int from, int to) {
        double hi = -std::numeric_limits<double>::infinity();
        for (const TrendPoint& point : points) {
            double sum = 0;
            bool any = false;
            for (int channel = from; channel < to && channel < point.values.size(); ++channel)
                if (std::isfinite(point.values[channel])) { sum += point.values[channel]; any = true; }
            if (any) hi = std::max(hi, sum);
        }
        return hi;
    };
    const double wear = highest(0, 4), energy = highest(4, 5), laps = highest(5, 6);
    const double lapMax = std::isfinite(laps) ? laps + 3 : 1;
    const double energyMax = std::isfinite(energy) ? energy + 0.5 : 1;
    // Fixed is each tyre's 0-100% scale, so four stacked tyres span 0-400%.
    const double wearMax = !dynamicTyres || !std::isfinite(wear) ? 400 : quarterAxisRange(0, wear, 0).max;
    return {
        {0, lapMax, lapAxisY, lapTooltipY, lapColor},
        {0, energyMax, energyAxisLabel, energyTooltipY, QColor()},
        {0, wearMax, percentAxisLabel, percentTooltip, QColor()},
    };
}

// The combined graph's three scales: lap time on the left, then MJ and tyre %.
QVector<TrendAxis> combinedAxes(const QVector<TrendPoint>& lapPoints, const QVector<TrendPoint>& tyrePoints,
                                bool showRecharge, const QColor& lapColor, bool dynamicTyres) {
    struct Extent { double lo = std::numeric_limits<double>::infinity(), hi = -std::numeric_limits<double>::infinity(); };
    const auto extent = [](const QVector<TrendPoint>& points, int from, int to) {
        Extent out;
        for (const TrendPoint& point : points)
            for (int i = from; i < to && i < point.values.size(); ++i)
                if (std::isfinite(point.values[i])) {
                    out.lo = std::min(out.lo, point.values[i]);
                    out.hi = std::max(out.hi, point.values[i]);
                }
        return out;
    };
    // Without recharge, the MJ scale fits ERS used alone.
    const Extent energy = extent(lapPoints, 0, showRecharge ? 2 : 1);
    const Extent laps = extent(lapPoints, 2, 3);
    const Extent tyres = extent(tyrePoints, 0, 4);
    // Lap time is read from 0:00.000 to 3 s above the slowest lap; energy from
    // 0 MJ to 0.5 MJ above the highest of ERS used and recharged.
    const double lapMax = std::isfinite(laps.hi) ? laps.hi + 3 : 1;
    const double energyMax = std::isfinite(energy.hi) ? energy.hi + 0.5 : 1;
    const TrendRange tyreRange = !dynamicTyres || !std::isfinite(tyres.lo)
        ? TrendRange{0, 100} : quarterAxisRange(tyres.lo, tyres.hi);
    return {
        {0, lapMax, lapAxisY, lapTooltipY, lapColor},
        {0, energyMax, energyAxisLabel, energyTooltipY, QColor()},
        {tyreRange.min, tyreRange.max, percentAxisLabel, percentTooltip, QColor()},
    };
}

QVector<TrendSeries> combinedSeries(bool showRecharge) {
    const TrendColors colors = trendColors();
    // Channels follow the lap points' values: used, recharged, lap time.
    QVector<TrendSeries> series{{QStringLiteral("Used"), colors.used, 1, true, 0, QString()}};
    if (showRecharge) series.push_back({QStringLiteral("Recharged"), colors.recharged, 1, true, 1, QString()});
    series.push_back({QStringLiteral("Lap time"), colors.lapTime, 0, true, 2, QString()});
    for (int corner = 0; corner < 4; ++corner)
        series.push_back({QString::fromLatin1(kCorners[corner]), cornerColor(corner), 2, false, -1, QString()});
    return series;
}

// Left to right in each lap: the four tyres stacked FL to RR from the
// bottom, ERS used, lap time.
QVector<TrendSeries> barSeries() {
    const TrendColors colors = trendColors();
    QVector<TrendSeries> series;
    for (int corner = 0; corner < 4; ++corner)
        series.push_back({QString::fromLatin1(kCorners[corner]), cornerColor(corner), 2, false, corner,
                          QStringLiteral("tyres")});
    series.push_back({QStringLiteral("Used"), colors.used, 1, false, 4, QString()});
    series.push_back({QStringLiteral("Lap time"), colors.lapTime, 0, false, 5, QString()});
    return series;
}

QVector<QColor> colorsOf(const QVector<TrendSeries>& series) {
    QVector<QColor> colors;
    for (const TrendSeries& item : series) colors.push_back(item.color);
    return colors;
}

} // namespace

TrendsPage::TrendsPage(SessionModel* model, QWidget* parent) : QWidget(parent), model_(model) {
    density_ = tnr::densityFromValue(
        settings_.value(tnr::compactKey(tnr::CompactSection::TrendsSummary), "normal"));
    life_ = settings_.value(QStringLiteral("ui/tyreWearMode"), "life").toString() != QLatin1String("wear");
    chartLayout_ = trendsChartLayoutFromKey(settings_.value(QStringLiteral("pageLayouts/trends"), "separate").toString());
    layout_ = loadLayout();

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    cardsBar_ = new QWidget;
    auto* cardsLayout = new QHBoxLayout(cardsBar_);
    cardsLayout->setContentsMargins(0, 0, 0, 0);
    cardsLayout->setSpacing(0);
    column->addWidget(cardsBar_);
    cardsDivider_ = tnrui::hline();
    column->addWidget(cardsDivider_);
    chartsLayout_ = new QVBoxLayout;
    chartsLayout_->setContentsMargins(0, 0, 0, 0);
    chartsLayout_->setSpacing(0);
    column->addLayout(chartsLayout_, 1);

    buildCards();
    buildCharts();
    applyLayout(layout_);

    if (model_) {
        connect(model_, &SessionModel::telemetryAppended, this, &TrendsPage::requestRefresh);
        connect(model_, &SessionModel::tyreAppended, this, &TrendsPage::requestRefresh);
        connect(model_, &SessionModel::lapsChanged, this, &TrendsPage::requestRefresh);
        connect(model_, &SessionModel::wasReset, this, &TrendsPage::requestRefresh);
        connect(model_, &SessionModel::chartConfigurationChanged, this, &TrendsPage::requestRefresh);
    }
}

void TrendsPage::buildCards() {
    QLayout* layout = cardsBar_->layout();
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    static const char* const kUnits[TrendsLayout::StatCount] = {"", "%/L", "MJ/L", "MJ/L", "kg/L", "", "", ""};
    for (int i = 0; i < TrendsLayout::StatCount; ++i) {
        cardSeps_[i] = nullptr;
        if (i > 0) { cardSeps_[i] = tnrui::vline(); layout->addWidget(cardSeps_[i]); }
        cards_[i] = new SummaryCard(QString::fromLatin1(TrendsLayout::statLabel(i)),
                                    QString::fromLatin1(kUnits[i]), density_);
        static_cast<QHBoxLayout*>(layout)->addWidget(cards_[i], 1);
    }
}

QComboBox* TrendsPage::makeRangeSelect(const QString& name, const char* settingsKey) {
    auto* select = new QComboBox;
    select->setFrame(false);
    select->setFixedHeight(26);
    select->setToolTip(name);
    select->setAccessibleName(name);
    select->addItem(QStringLiteral("Stint Laps"), QStringLiteral("stint"));
    select->addItem(QStringLiteral("All Laps"), QStringLiteral("all"));
    const QString key = QString::fromLatin1(settingsKey);
    select->setCurrentIndex(settings_.value(key, "stint").toString() == QLatin1String("all") ? 1 : 0);
    connect(select, QOverload<int>::of(&QComboBox::activated), this, [this, select, key](int) {
        settings_.setValue(key, select->currentData().toString());
        requestRefresh();
    });
    return select;
}

void TrendsPage::buildCharts() {
    // The chart area is rebuilt whole: each layout holds a different set of charts.
    if (chartsArea_) {
        chartsLayout_->removeWidget(chartsArea_);
        delete chartsArea_;
    }
    lapChart_ = usedChart_ = rechargeChart_ = singleChart_ = nullptr;
    tyreChart_ = nullptr;
    ersRow_ = nullptr;
    ersDivider_ = nullptr;
    lapRange_ = usedRange_ = rechargeRange_ = tyreRange_ = combinedRange_ = nullptr;

    chartsArea_ = new QWidget;
    auto* area = new QVBoxLayout(chartsArea_);
    area->setContentsMargins(0, 0, 0, 0);
    area->setSpacing(0);
    chartsLayout_->addWidget(chartsArea_, 1);

    const TrendColors colors = trendColors();
    const bool* charts = layout_.charts;
    if (chartLayout_ == TrendsChartLayout::Separate) {
        // Only the charts the Layout Editor shows take a row; rows share the
        // height 1 : 1 : 1.1 (lap times, ERS, tyre).
        const auto divider = [area]() {
            if (area->count() > 0) area->addWidget(tnrui::hline());
        };
        // Shared-tooltip order: top to bottom, ahead of the tyre graph.
        if (charts[TrendsLayout::LapTimes]) {
            TrendChart::Config config;
            config.title = QStringLiteral("Lap Times");
            config.series = {{QStringLiteral("Lap time"), colors.lapTime, 0, false, -1, QString()}};
            config.discrete = true;
            config.formatY = lapAxisY;
            config.formatTooltipY = lapTooltipY;
            config.syncOrder = 1;
            lapChart_ = new TrendChart(config);
            lapRange_ = makeRangeSelect(QStringLiteral("Lap times range"), "ui/trendsLapTimeRange");
            lapChart_->setHeaderControl(lapRange_);
            area->addWidget(lapChart_, 10);
        }
        if (charts[TrendsLayout::ErsUsage] || charts[TrendsLayout::Recharge]) {
            divider();
            ersRow_ = new QWidget;
            auto* row = new QHBoxLayout(ersRow_);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);
            const auto energyConfig = [](const QString& title, const QString& label, const QColor& color, int order) {
                TrendChart::Config config;
                config.title = title;
                config.series = {{label, color, 0, false, -1, QString()}};
                config.discrete = true;
                config.zeroBaselineHeadroom = 0.5;
                config.formatY = energyAxisY;
                config.formatTooltipY = energyTooltipY;
                config.syncOrder = order;
                return config;
            };
            if (charts[TrendsLayout::ErsUsage]) {
                usedChart_ = new TrendChart(energyConfig(QStringLiteral("ERS Usage"), QStringLiteral("Used"), colors.used, 2));
                usedRange_ = makeRangeSelect(QStringLiteral("ERS usage range"), "ui/trendsErsUsageRange");
                usedChart_->setHeaderControl(usedRange_);
                row->addWidget(usedChart_, 1);
            }
            if (charts[TrendsLayout::ErsUsage] && charts[TrendsLayout::Recharge]) {
                ersDivider_ = tnrui::vline();
                row->addWidget(ersDivider_);
            }
            if (charts[TrendsLayout::Recharge]) {
                rechargeChart_ = new TrendChart(energyConfig(QStringLiteral("Recharge"), QStringLiteral("Recharged"), colors.recharged, 3));
                rechargeRange_ = makeRangeSelect(QStringLiteral("Recharge range"), "ui/trendsRechargeRange");
                rechargeChart_->setHeaderControl(rechargeRange_);
                row->addWidget(rechargeChart_, 1);
            }
            area->addWidget(ersRow_, 10);
        }
        if (charts[TrendsLayout::TyreWear]) {
            divider();
            tyreChart_ = new TrendTyreChart;
            tyreChart_->setLifeMode(life_);
            tyreRange_ = makeRangeSelect(QStringLiteral("Tyre graph range"), "ui/stintTyreRange");
            tyreChart_->setPanelHeaderControl(0, tyreRange_);
            area->addWidget(tyreChart_, 11);
        }
        if (area->count() == 0) area->addStretch(1);
    } else {
        const bool bars = chartLayout_ == TrendsChartLayout::Bars;
        if (charts[bars ? TrendsLayout::Bars : TrendsLayout::Combined]) {
            TrendChart::Config config;
            config.axisCount = 3;
            config.formatY = lapAxisY;
            if (bars) {
                config.title = barsTitle(life_);
                config.series = barSeries();
                config.bars = true;
                config.discrete = true;
            } else {
                config.title = combinedTitle(life_);
                config.series = combinedSeries(chartLayout_ != TrendsChartLayout::CombinedNoRecharge);
            }
            singleChart_ = new TrendChart(config);
            combinedRange_ = makeRangeSelect(bars ? QStringLiteral("Bar graph range") : QStringLiteral("Combined graph range"),
                                             "ui/trendsCombinedRange");
            singleChart_->setHeaderControl(combinedRange_);
            area->addWidget(singleChart_, 1);
        } else {
            area->addStretch(1);
        }
    }
    requestRefresh();
}

TrendsLayout TrendsPage::loadLayout() {
    TrendsLayout layout;
    settings_.beginGroup(QStringLiteral("trendsLayout"));
    settings_.beginGroup(QStringLiteral("statsCards"));
    for (int i = 0; i < TrendsLayout::StatCount; ++i)
        layout.statsCards[i] = settings_.value(TrendsLayout::statKey(i), true).toBool();
    settings_.endGroup();
    settings_.beginGroup(QStringLiteral("charts"));
    for (int i = 0; i < TrendsLayout::ChartCount; ++i)
        layout.charts[i] = settings_.value(TrendsLayout::chartKey(i), true).toBool();
    settings_.endGroup();
    settings_.endGroup();
    return layout;
}

void TrendsPage::saveLayout(const TrendsLayout& layout) {
    settings_.beginGroup(QStringLiteral("trendsLayout"));
    settings_.beginGroup(QStringLiteral("statsCards"));
    for (int i = 0; i < TrendsLayout::StatCount; ++i)
        settings_.setValue(TrendsLayout::statKey(i), layout.statsCards[i]);
    settings_.endGroup();
    settings_.beginGroup(QStringLiteral("charts"));
    for (int i = 0; i < TrendsLayout::ChartCount; ++i)
        settings_.setValue(TrendsLayout::chartKey(i), layout.charts[i]);
    settings_.endGroup();
    settings_.endGroup();
}

void TrendsPage::applyLayout(const TrendsLayout& layout) {
    bool any = false;
    for (int i = 0; i < TrendsLayout::StatCount; ++i) {
        if (cards_[i]) cards_[i]->setVisible(layout.statsCards[i]);
        // A separator shows only between two visible cards.
        if (cardSeps_[i]) cardSeps_[i]->setVisible(layout.statsCards[i] && any);
        any = any || layout.statsCards[i];
    }
    cardsBar_->setVisible(any);
    cardsDivider_->setVisible(any);
}

void TrendsPage::applyAndSaveLayout(const TrendsLayout& layout) {
    const bool chartsChanged = !std::equal(std::begin(layout.charts), std::end(layout.charts),
                                           std::begin(layout_.charts));
    layout_ = layout;
    saveLayout(layout);
    applyLayout(layout);
    if (chartsChanged) buildCharts();
    requestRefresh();
}

TrendsChartLayout TrendsPage::chartLayout() const { return chartLayout_; }

void TrendsPage::setChartLayout(TrendsChartLayout layout) {
    if (chartLayout_ == layout) return;
    chartLayout_ = layout;
    settings_.setValue(QStringLiteral("pageLayouts/trends"), trendsChartLayoutKey(layout));
    buildCharts();
}

void TrendsPage::setDensityMode(tnr::DensityMode mode) {
    if (density_ == mode) return;
    density_ = mode;
    buildCards();
    applyLayout(layout_);
    requestRefresh();
}

void TrendsPage::setDriver(int carIdx) {
    if (driver_ == carIdx) return;
    driver_ = carIdx;
    history_.reset();
    requestRefresh();
}

void TrendsPage::setLapHistory(const tnrp::DriverLapHistoryRow& history) {
    if (history.car_idx != driver_) return;
    history_ = history;
    requestRefresh();
}

void TrendsPage::clearLapHistory() {
    history_.reset();
    requestRefresh();
}

void TrendsPage::update(const StatusRow* status, const tnrp::TyreSetsRow* tyreSets, const LapRow* lap) {
    if (status) status_ = *status; else status_.reset();
    if (tyreSets) tyreSets_ = *tyreSets; else tyreSets_.reset();
    if (lap && lap->lap_num != kPlaybackMissingInt) currentLap_ = lap->lap_num;
    else currentLap_.reset();
    requestRefresh();
}

void TrendsPage::setHasMguh(bool hasMguh) {
    if (hasMguh_ == hasMguh) return;
    hasMguh_ = hasMguh;
    requestRefresh();
}

void TrendsPage::setTyreLifeMode(bool life) {
    if (life_ == life) return;
    life_ = life;
    if (tyreChart_) tyreChart_->setLifeMode(life);
    if (singleChart_)
        singleChart_->setTitle(chartLayout_ == TrendsChartLayout::Bars ? barsTitle(life) : combinedTitle(life));
    requestRefresh();
}

void TrendsPage::setPlaybackMode(bool on, float currentTime) {
    playback_ = on;
    currentTime_ = currentTime;
    requestRefresh();
}

void TrendsPage::setCurrentTime(float t) {
    currentTime_ = t;
    requestRefresh();
}

void TrendsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    requestRefresh();
}

void TrendsPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::ApplicationPaletteChange) return;
    // Every series colour has a light-theme variant.
    const TrendColors colors = trendColors();
    if (lapChart_) lapChart_->setSeriesColors({colors.lapTime});
    if (usedChart_) usedChart_->setSeriesColors({colors.used});
    if (rechargeChart_) rechargeChart_->setSeriesColors({colors.recharged});
    if (tyreChart_) tyreChart_->refreshColors();
    if (singleChart_)
        singleChart_->setSeriesColors(colorsOf(chartLayout_ == TrendsChartLayout::Bars
            ? barSeries() : combinedSeries(chartLayout_ != TrendsChartLayout::CombinedNoRecharge)));
    requestRefresh();
}

void TrendsPage::requestRefresh() {
    dirty_ = true;
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
}

// Every value comes from rows the model and MainWindow already hold. The
// summary covers the current stint; each graph can show all laps.
void TrendsPage::refresh() {
    if (!model_) return;
    const SessionData& data = model_->data();

    // Lap start times of the streamed driver, as the chart axes label them.
    QVector<TrendLapBoundary> boundaries;
    boundaries.reserve(data.laps.size() + 1);
    for (const LapBlock& lap : data.laps) boundaries.push_back({lap.lapNum, lap.startSessionTime});
    if (data.curLapNum >= 0) boundaries.push_back({data.curLap.lapNum, data.curLap.startSessionTime});
    std::stable_sort(boundaries.begin(), boundaries.end(),
        [](const TrendLapBoundary& a, const TrendLapBoundary& b) { return a.sessionTime < b.sessionTime; });

    // Completed laps and the stint boundary arrive as a push whenever they change.
    const bool haveHistory = history_ && history_->car_idx == driver_;
    const int stintStart = haveHistory && history_->stint_start_lap > 0 ? history_->stint_start_lap : 1;
    QVector<Lap> allLaps;
    if (haveHistory) allLaps = QVector<Lap>(history_->laps.cbegin(), history_->laps.cend());
    QVector<Lap> stintLaps;
    QVector<int> lapNums;
    for (const Lap& lap : allLaps) {
        lapNums.push_back(lap.lap_num);
        if (lap.lap_num >= stintStart) stintLaps.push_back(lap);
    }
    const auto lapsFor = [&](QComboBox* select) -> const QVector<Lap>& {
        return select && select->currentData().toString() == QLatin1String("all") ? allLaps : stintLaps;
    };

    // The status history grows in place; the scan reads only what is new and
    // starts again when held rows were rewritten.
    const StintStatusScan& scan = scanner_.scanFor(data.stsBuf, model_->playbackDataRevision());
    const QHash<int, TrendLapMeasure> measures = measureTrendLaps(scan, data.stsBuf, boundaries, lapNums, hasMguh_);
    refreshCards(stintLaps, haveHistory ? stintStart : -1, measures);

    const bool dynamicTyres = model_->dynamicYAxis(tnr::GraphSection::TyreWear);
    const bool sync = model_->cursorSync();
    const bool secondaryV = model_->secondaryVerticalCrosshair();
    const bool secondaryH = model_->secondaryHorizontalCrosshair();
    const TrendColors colors = trendColors();

    if (lapChart_) {
        // Fastest marks the quickest valid lap among those the graph shows.
        const QVector<Lap>& laps = lapsFor(lapRange_);
        const std::optional<int> shownFastest = fastestValid(laps);
        QVector<TrendPoint> points;
        for (const Lap& lap : laps) {
            TrendPoint point;
            point.x = lap.lap_num;
            point.values = {lapSeconds(lap)};
            point.label = QStringLiteral("Lap %1").arg(lap.lap_num);
            point.invalid = lap.lap_time_ms > 0 && !lap.lap_valid;
            point.fastest = lap.lap_valid && shownFastest && lap.lap_time_ms == *shownFastest;
            point.lapStart = trendLapStart(boundaries, lap.lap_num);
            points.push_back(point);
        }
        lapChart_->setData(points);
        lapChart_->applyCursorSync(sync, secondaryV, secondaryH);
    }
    if (usedChart_) {
        usedChart_->setData(energyPoints(true, lapsFor(usedRange_), measures, boundaries));
        usedChart_->applyCursorSync(sync, secondaryV, secondaryH);
    }
    if (rechargeChart_) {
        rechargeChart_->setData(energyPoints(false, lapsFor(rechargeRange_), measures, boundaries));
        rechargeChart_->applyCursorSync(sync, secondaryV, secondaryH);
    }
    if (tyreChart_) {
        // Stint Laps start at the Session History stint's first lap, else the
        // model's tyre-change heuristic; All Laps at the first lap.
        const bool all = tyreRange_ && tyreRange_->currentData().toString() == QLatin1String("all");
        double lower = 0;
        if (!all) {
            const double known = haveHistory && history_->stint_start_lap > 0
                ? trendLapStart(boundaries, history_->stint_start_lap) : kNaN;
            lower = std::isfinite(known) ? known : data.currentStintStartTime;
        }
        if (!boundaries.isEmpty()) lower = std::max(lower, boundaries.first().sessionTime);
        const float endTime = playback_ ? currentTime_ : data.latestTime;
        tyreChart_->refresh(data, boundaries, endTime, lower, dynamicTyres, model_->playbackDataRevision());
        tyreChart_->setCursorSync(sync, secondaryV, secondaryH);
    }
    if (singleChart_) {
        const bool all = combinedRange_ && combinedRange_->currentData().toString() == QLatin1String("all");
        const QVector<Lap>& laps = all ? allLaps : stintLaps;
        if (chartLayout_ == TrendsChartLayout::Bars) {
            const QVector<TrendPoint> points = buildBarPoints(laps, measures, boundaries, data.damageBuf, life_);
            singleChart_->setData(points, {}, barAxes(points, colors.lapTime, dynamicTyres));
        } else {
            const int firstLap = all ? (allLaps.isEmpty() ? 1 : allLaps.first().lap_num) : stintStart;
            const QVector<TrendPoint> lapPoints = buildLapEndPoints(laps, measures);
            const QVector<TrendPoint> tyrePoints =
                buildTyreSamples(laps, firstLap, currentLap_, boundaries, data.damageBuf, life_);
            singleChart_->setData(tyrePoints, lapPoints,
                combinedAxes(lapPoints, tyrePoints, chartLayout_ != TrendsChartLayout::CombinedNoRecharge,
                             colors.lapTime, dynamicTyres));
        }
    }
}

// The summary strip. `stintStart` is -1 while no lap history is known.
void TrendsPage::refreshCards(const QVector<tnrp::SessionHistoryLap>& stintLaps, int stintStart,
                              const QHash<int, TrendLapMeasure>& measures) {
    const bool haveHistory = stintStart >= 0;
    const TrendColors colors = trendColors();
    const auto set = [this](TrendsLayout::StatCard card, const QString& value, const QColor& color,
                            const QString& title, const QString& sub) {
        SummaryCard* target = cards_[card];
        if (!target) return;
        target->setValue(value, color);
        if (target->toolTip() != title) target->setToolTip(title);
        target->setSub(sub);
    };

    set(TrendsLayout::StintLaps, haveHistory ? QString::number(stintLaps.size()) : SummaryCard::kMissing, QColor(),
        haveHistory ? QStringLiteral("Current stint starts on lap %1").arg(stintStart) : QString(),
        haveHistory ? QStringLiteral("From lap %1").arg(stintStart) : QString());

    std::optional<double> wearPerLap;
    if (tyreSets_)
        for (const tnrp::TyreSet& tyreSet : tyreSets_->sets)
            if (tyreSet.fitted) { wearPerLap = tyreSet.avg_wear_per_lap; break; }
    set(TrendsLayout::WearPerLap, wearPerLap ? QString::number(*wearPerLap, 'f', 2) : SummaryCard::kMissing, QColor(),
        QStringLiteral("Average wear per lap on the fitted set in this session; same calculation as the Tyres page."),
        QStringLiteral("Fitted set, this session"));

    QVector<double> deployed, harvested, fuel;
    for (const Lap& lap : stintLaps) {
        const TrendLapMeasure measure = measures.value(lap.lap_num);
        deployed.push_back(measure.deployedMj);
        harvested.push_back(measure.harvestedMj);
        fuel.push_back(measure.fuelKg);
    }
    const Average harvestedAverage = average(harvested);
    const Average deployedAverage = average(deployed);
    const Average fuelAverage = average(fuel);
    set(TrendsLayout::RecPerLap, harvestedAverage.value, colors.recharged, harvestedAverage.title, harvestedAverage.coverage);
    set(TrendsLayout::ErsPerLap, deployedAverage.value, colors.used, deployedAverage.title, deployedAverage.coverage);
    set(TrendsLayout::FuelPerLap, fuelAverage.value, QColor(), fuelAverage.title, fuelAverage.coverage);

    const Lap* fastest = nullptr;
    for (const Lap& lap : stintLaps)
        if (lap.lap_valid && lap.lap_time_ms > 0 && (!fastest || lap.lap_time_ms < fastest->lap_time_ms)) fastest = &lap;
    set(TrendsLayout::FastestLap, fastest ? fmtMs(fastest->lap_time_ms) : SummaryCard::kMissing,
        tnr::themed("#BF5FFF", "#7C3BA6"), QStringLiteral("Fastest valid lap in the current stint"),
        fastest ? QStringLiteral("Lap %1").arg(fastest->lap_num) : QString());

    const Lap* previous = stintLaps.isEmpty() ? nullptr : &stintLaps.last();
    set(TrendsLayout::PreviousLap,
        previous && previous->lap_time_ms > 0 ? fmtMs(previous->lap_time_ms) : SummaryCard::kMissing,
        previous && !previous->lap_valid ? QColor("#C4162A") : QColor(),
        previous && !previous->lap_valid ? QStringLiteral("Previous completed lap · Invalid")
                                         : QStringLiteral("Previous completed lap"),
        previous ? QStringLiteral("Lap %1%2").arg(previous->lap_num)
                       .arg(previous->lap_valid ? QString() : QStringLiteral(" · Invalid"))
                 : QString());

    const int compound = status_ ? status_->tyre_compound : 0;
    const int visual = status_ ? status_->visual_compound : 0;
    const bool knownAge = status_ && status_->tyre_age_laps != kPlaybackMissingInt;
    set(TrendsLayout::Tyre, compound > 0 ? tnr::Ln(QStringLiteral("tyre.actual"), compound) : SummaryCard::kMissing,
        compound > 0 ? tyreTextColor(compound, visual) : QColor(), QString(),
        knownAge ? QStringLiteral("%1L age").arg(status_->tyre_age_laps) : QString());
}
