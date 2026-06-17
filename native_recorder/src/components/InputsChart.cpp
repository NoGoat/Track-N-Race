#include "InputsChart.h"
#include "../SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_THROTTLE("#2FC584"), C_BRAKE("#FF4040");
}

InputsChart::InputsChart(QWidget* parent)
    : ChartView(parent)
{
    axXId_ = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0, true });
    int axPct = addAxis({ Side::Left, -1.0, 1.0, QColor(), true,  'f', 2 });

    setAxisTimeTicker(axXId_, "%m:%s");

    thId_ = addSeries({ "Throttle", C_THROTTLE, 2.0, axXId_, axPct, "", 2, false, true });
    brId_ = addSeries({ "Brake",    C_BRAKE,    2.0, axXId_, axPct, "", 2, false, true });

    setHoverReadout(true);
    setLegendVisible(false);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
    refreshTimer_->start();
}

void InputsChart::showEvent(QShowEvent* e) {
    ChartView::showEvent(e);
    requestRefresh();
}

void InputsChart::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &InputsChart::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &InputsChart::requestRefresh);
    requestRefresh();
}

void InputsChart::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void InputsChart::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void InputsChart::setWindowSeconds(float seconds) { windowS_ = seconds; requestRefresh(); }
void InputsChart::requestRefresh() { dirty_ = true; }

float InputsChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void InputsChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    setSeriesData(id, xs, ys);
}

void InputsChart::refresh() {
    if (!model_) return;
    buildDefault(currentTime());
    requestReplot();
}

void InputsChart::buildDefault(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(thId_); clear(brId_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.telBuf.begin(), lb(d.telBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.telBuf.size(); ++i) {
        const auto& s = d.telBuf[i];
        if (s.t > endTime) break;
        appendPoint(thId_, s.t, s.throttle);
        appendPoint(brId_, s.t, -s.brake);
        lastAddedTime_ = s.t;
    }
    trimBefore(thId_, left);
    trimBefore(brId_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}
