#include "TyreChartsWidget.h"
#include "ChartView.h"
#include "../SessionModel.h"

#include <QGridLayout>
#include <QColor>
#include <QTimer>
#include <QShowEvent>
#include <algorithm>

namespace {
// Per-corner colors matching the Electron TyreTrendCharts
const QColor C_FL("#e10600");   // Front Left  — red
const QColor C_FR("#4488ff");   // Front Right — blue
const QColor C_RL("#37872D");   // Rear Left   — green
const QColor C_RR("#ffd700");   // Rear Right  — gold
const QColor kWheelColors[4] = { C_FL, C_FR, C_RL, C_RR };
const char*  kWheelNames[4]  = { "FL", "FR", "RL", "RR" };
}

TyreChartsWidget::TyreChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);

    auto buildChart = [&](const QString& yLabel, double yMin, double yMax,
                          const QString& unit, ChartView*& outChart, int* outIds, int& outXId) {
        outChart = new ChartView;
        outXId = outChart->addAxis({ ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true });
        int yId = outChart->addAxis({ ChartView::Side::Left, yMin, yMax, QColor(), true, 'f', 0 });
        outChart->setAxisTimeTicker(outXId, "%m:%s");
        for (int w = 0; w < 4; ++w) {
            outIds[w] = outChart->addSeries({
                kWheelNames[w], kWheelColors[w], 1.5, outXId, yId, unit, 0
            });
        }
        outChart->setHoverReadout(true);
        outChart->setLegendVisible(true);
        return outChart;
    };

    surfChart_  = buildChart("°C",  0, 200, "°C", surfChart_,  surfIds_,  surfXId_);
    innerChart_ = buildChart("°C",  0, 200, "°C", innerChart_, innerIds_, innerXId_);
    brakeChart_ = buildChart("°C",  0, 1200,"°C", brakeChart_, brakeIds_, brakeXId_);
    wearChart_  = buildChart("%",   0, 100, "%",  wearChart_,  wearIds_,  wearXId_);

    grid->addWidget(surfChart_,  0, 0);
    grid->addWidget(innerChart_, 0, 1);
    grid->addWidget(brakeChart_, 1, 0);
    grid->addWidget(wearChart_,  1, 1);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
    refreshTimer_->start();
}

void TyreChartsWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    requestRefresh();
}

void TyreChartsWidget::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::tyreAppended, this, &TyreChartsWidget::requestRefresh);
    connect(m, &SessionModel::wasReset,     this, &TyreChartsWidget::requestRefresh);
    requestRefresh();
}

void TyreChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void TyreChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void TyreChartsWidget::setWindowSeconds(float s) { windowS_ = s; requestRefresh(); }
void TyreChartsWidget::requestRefresh() { dirty_ = true; }

float TyreChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void TyreChartsWidget::refresh() {
    if (!model_) return;

    const SessionData& d = model_->data();
    const float endTime = currentTime();
    const float left    = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        for (int w = 0; w < 4; ++w) {
            surfChart_->clear(surfIds_[w]);
            innerChart_->clear(innerIds_[w]);
            brakeChart_->clear(brakeIds_[w]);
            wearChart_->clear(wearIds_[w]);
        }
        lastAddedTime_ = left;
    }

    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t,
            [](const auto& s, float key) { return s.t < key; });
    };

    int startIdx = (int)std::distance(d.tyreBuf.begin(), lb(d.tyreBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIdx; i < d.tyreBuf.size(); ++i) {
        const auto& s = d.tyreBuf[i];
        if (s.t > endTime) break;

        surfChart_->appendPoint(surfIds_[0], s.t, s.surfFl);
        surfChart_->appendPoint(surfIds_[1], s.t, s.surfFr);
        surfChart_->appendPoint(surfIds_[2], s.t, s.surfRl);
        surfChart_->appendPoint(surfIds_[3], s.t, s.surfRr);

        innerChart_->appendPoint(innerIds_[0], s.t, s.innerFl);
        innerChart_->appendPoint(innerIds_[1], s.t, s.innerFr);
        innerChart_->appendPoint(innerIds_[2], s.t, s.innerRl);
        innerChart_->appendPoint(innerIds_[3], s.t, s.innerRr);

        brakeChart_->appendPoint(brakeIds_[0], s.t, s.brakeFl);
        brakeChart_->appendPoint(brakeIds_[1], s.t, s.brakeFr);
        brakeChart_->appendPoint(brakeIds_[2], s.t, s.brakeRl);
        brakeChart_->appendPoint(brakeIds_[3], s.t, s.brakeRr);

        wearChart_->appendPoint(wearIds_[0], s.t, s.wearFl);
        wearChart_->appendPoint(wearIds_[1], s.t, s.wearFr);
        wearChart_->appendPoint(wearIds_[2], s.t, s.wearRl);
        wearChart_->appendPoint(wearIds_[3], s.t, s.wearRr);

        lastAddedTime_ = s.t;
    }

    for (int w = 0; w < 4; ++w) {
        surfChart_->trimBefore(surfIds_[w],  left);
        innerChart_->trimBefore(innerIds_[w], left);
        brakeChart_->trimBefore(brakeIds_[w], left);
        wearChart_->trimBefore(wearIds_[w],  left);
    }

    const double lo = (double)std::max(0.0f, left);
    const double hi = (double)std::max(windowS_, endTime);
    const double hiClamped = hi > lo ? hi : lo + 1.0;
    surfChart_->setXRange(surfXId_,   lo, hiClamped);
    innerChart_->setXRange(innerXId_, lo, hiClamped);
    brakeChart_->setXRange(brakeXId_, lo, hiClamped);
    wearChart_->setXRange(wearXId_,  lo, hiClamped);

    surfChart_->requestReplot();
    innerChart_->requestReplot();
    brakeChart_->requestReplot();
    wearChart_->requestReplot();

    prevEndTime_ = endTime;
}
