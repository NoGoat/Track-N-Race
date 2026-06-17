#include "GearChart.h"
#include "../SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_GEAR("#5794F2");
}

GearChart::GearChart(QWidget* parent)
    : ChartView(parent)
{
    axXId_ = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0, true });
    int axGear = addAxis({ Side::Left, 0.0, 9.0, QColor(), true,  'f', 0 });

    setAxisTimeTicker(axXId_, "%m:%s");

    addBand({ axGear, 0.0, 2.5, QColor(196, 22, 42, 51) });
    addBand({ axGear, 2.5, 4.5, QColor(212, 173, 4, 51) });
    addBand({ axGear, 4.5, 6.5, QColor(0, 200, 83, 51) });
    addBand({ axGear, 6.5, 9.0, QColor(31, 96, 196, 51) });

    grId_ = addSeries({ "Gear", C_GEAR, 2.0, axXId_, axGear, "", 0, false, false, QColor(), true });

    setHoverReadout(true);
    setLegendVisible(false);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
    refreshTimer_->start();
}

void GearChart::showEvent(QShowEvent* e) {
    ChartView::showEvent(e);
    requestRefresh();
}

void GearChart::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &GearChart::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &GearChart::requestRefresh);
    requestRefresh();
}

void GearChart::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void GearChart::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void GearChart::setWindowSeconds(float seconds) { windowS_ = seconds; prevEndTime_ = -9999.0f; requestRefresh(); }
void GearChart::requestRefresh() { dirty_ = true; }

float GearChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void GearChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    setSeriesData(id, xs, ys);
}

void GearChart::refresh() {
    if (!model_) return;
    buildDefault(currentTime());
    requestReplot();
}

void GearChart::buildDefault(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(grId_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.telBuf.begin(), lb(d.telBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.telBuf.size(); ++i) {
        const auto& s = d.telBuf[i];
        if (s.t > endTime) break;
        appendPoint(grId_, s.t, s.gear);
        lastAddedTime_ = s.t;
    }
    trimBefore(grId_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}
