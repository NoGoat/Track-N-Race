#include "TrendTyreChart.h"
#include "CardColors.h"
#include "../SessionModel.h"

#include <QLabel>
#include <QResizeEvent>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {
// Corner colours; Electron darkens FR/RL/RR on its light theme for contrast.
QColor cornerColor(int corner) {
    switch (corner) {
        case 0:  return QColor("#e10600");
        case 1:  return tnr::themed("#4488ff", "#0B57D0");
        case 2:  return tnr::themed("#37872D", "#137333");
        default: return tnr::themed("#ffd700", "#765900");
    }
}
const char* const kCornerNames[4] = {"FL", "FR", "RL", "RR"};
} // namespace

TrendTyreChart::TrendTyreChart(QWidget* parent) : ChartView(parent) {
    xId_ = addAxis({ Side::Bottom, 0.0, 1.0, QColor(), true, 'f', 0, true, 60 });
    yId_ = addAxis({ Side::Left, 0.0, 100.0, QColor(), true, 'f', 0, true });
    setAxisTimeTicker(xId_, "%m:%s");
    // Electron TyreTrendCharts: dashed 3/3 grid on both axes, 3 px x ticks.
    setAxisGridStyle(xId_, true, 3);
    setAxisGridStyle(yId_, true);
    setAxisNumberSuffix(yId_, 1.0, QStringLiteral("%"));
    setPanelTitle(0, QStringLiteral("TYRE LIFE"));
    setPanelLegendVisible(0, true);
    for (int corner = 0; corner < 4; ++corner) {
        SeriesSpec spec;
        spec.name = kCornerNames[corner];
        spec.color = cornerColor(corner);
        spec.width = 1.5;
        spec.xAxisId = xId_;
        spec.yAxisId = yId_;
        spec.unit = QStringLiteral("%");
        spec.tipPrecision = 1;   // Electron toFixed(1)
        // All Laps and Stint Laps draw continuous series with native lines, as Electron.
        spec.lineType = LineType::NativeLine;
        ids_[corner] = addSeries(spec);
    }
    // The Tyres page's wear chart order in the shared tooltip.
    setPanelSyncOrder(0, 40);
    setHoverReadout(true);

    empty_ = new QLabel(QStringLiteral("No data"), this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setForegroundRole(QPalette::PlaceholderText);
    empty_->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_->hide();
}

void TrendTyreChart::setLifeMode(bool life) {
    if (life_ == life) return;
    life_ = life;
    setPanelTitle(0, life ? QStringLiteral("TYRE LIFE") : QStringLiteral("TYRE WEAR"));
    dataKey_.clear();   // re-read every sample with the new mapping
}

void TrendTyreChart::refreshColors() {
    for (int corner = 0; corner < 4; ++corner) setSeriesColor(ids_[corner], cornerColor(corner));
    requestReplot();
}

void TrendTyreChart::resizeEvent(QResizeEvent* event) {
    ChartView::resizeEvent(event);
    empty_->setGeometry(rect());
    empty_->raise();
}

void TrendTyreChart::refresh(const SessionData& data, const QVector<TrendLapBoundary>& boundaries,
                             float endTime, double lower, bool dynamicY, quint64 revision) {
    const double upper = std::max(lower + 1.0, double(endTime));
    setXRange(xId_, lower, upper);
    // Lap numbers at the lap lines, as the All Laps / Stint Laps axes label them.
    QVector<double> ticks;
    QStringList labels;
    for (const TrendLapBoundary& boundary : boundaries) {
        if (boundary.sessionTime < lower || boundary.sessionTime > upper) continue;
        ticks.push_back(boundary.sessionTime);
        labels.push_back(QString::number(boundary.lapNum));
    }
    if (!ticks.isEmpty()) setAxisLabelMap(xId_, ticks, labels, true);
    else setAxisTimeTicker(xId_, "%m:%s");

    const auto value = [this](float wear) { return life_ ? 100.0f - wear : wear; };
    const QString key = QStringLiteral("%1|%2|%3").arg(lower, 0, 'f', 3).arg(life_ ? 1 : 0).arg(revision);
    const bool rebuild = key != dataKey_ || endTime < prevEndTime_ || std::abs(endTime - prevEndTime_) > 1.0f;
    if (rebuild) {
        for (int corner = 0; corner < 4; ++corner) clear(ids_[corner]);
        lastAddedTime_ = float(lower) - 0.0001f;
        dataKey_ = key;
    }
    const SampleRange<DamageSample> rows = data.damage();
    auto it = std::lower_bound(rows.cbegin(), rows.cend(), lastAddedTime_ + 0.0001f,
        [](const DamageSample& sample, float time) { return sample.t < time; });
    for (; it != rows.cend(); ++it) {
        if (it->t > endTime) break;
        if (it->t < lower) continue;
        const float wear[4] = {it->wearFl, it->wearFr, it->wearRl, it->wearRr};
        for (int corner = 0; corner < 4; ++corner) appendPoint(ids_[corner], it->t, value(wear[corner]));
        lastAddedTime_ = it->t;
    }
    prevEndTime_ = endTime;
    fitAxisToVisibleSeries(yId_, {ids_[0], ids_[1], ids_[2], ids_[3]}, 0.0, 100.0, dynamicY, false);

    // A single sample can't draw a line.
    const bool hasData = rows.size() > 1;
    empty_->setVisible(!hasData);
    if (!hasData) empty_->raise();
    requestReplot();
}
