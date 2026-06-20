#include "SteeringChart.h"
#include "../SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_STEER("#BF5FFF");
}

SteeringChart::SteeringChart(QWidget* parent)
    : ChartView(parent)
{
    axXId_ = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0, true, true });
    int axPct = addAxis({ Side::Left, -1.0, 1.0, QColor(), true,  'f', 2 });

    stId_ = addSeries({ "Steering", C_STEER, 2.0, axXId_, axPct, "", 2, false });

    // Add a center zero-line reference series manually or rely on ChartView's graticule.
    // ChartView already draws the zero line.

    setHoverReadout(true);
    setLegendVisible(false);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
    refreshTimer_->start();
}

void SteeringChart::showEvent(QShowEvent* e) {
    ChartView::showEvent(e);
    requestRefresh();
}

void SteeringChart::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &SteeringChart::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &SteeringChart::requestRefresh);
    requestRefresh();
}

void SteeringChart::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void SteeringChart::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void SteeringChart::setWindowSeconds(float seconds) { windowS_ = seconds; prevEndTime_ = -9999.0f; requestRefresh(); }
void SteeringChart::requestRefresh() { dirty_ = true; }

float SteeringChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void SteeringChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    setSeriesData(id, xs, ys);
}

void SteeringChart::refresh() {
    if (!model_) return;
    buildDefault(currentTime());
    requestReplot();
}

void SteeringChart::buildDefault(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(stId_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.telBuf.begin(), lb(d.telBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.telBuf.size(); ++i) {
        const auto& s = d.telBuf[i];
        if (s.t > endTime) break;
        appendPoint(stId_, s.t, s.steering);
        lastAddedTime_ = s.t;
    }
    trimBefore(stId_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}
