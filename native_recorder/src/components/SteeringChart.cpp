#include "SteeringChart.h"
#include "../SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_STEER("#BF5FFF");

void decimate(const QVector<double>& xs, const QVector<double>& ys, int targetCols,
              QVector<double>& ox, QVector<double>& oy) {
    const int n = xs.size();
    if (targetCols <= 0 || n <= targetCols * 2) { ox = xs; oy = ys; return; }
    const double bucket = double(n) / targetCols;
    ox.clear(); oy.clear();
    ox.reserve(targetCols * 2); oy.reserve(targetCols * 2);
    for (int c = 0; c < targetCols; ++c) {
        const int start = int(c * bucket);
        int end = int((c + 1) * bucket);
        if (end > n) end = n;
        if (start >= end) continue;
        int lo = start, hi = start;
        for (int i = start + 1; i < end; ++i) {
            if (ys[i] < ys[lo]) lo = i;
            if (ys[i] > ys[hi]) hi = i;
        }
        const int a = std::min(lo, hi), b = std::max(lo, hi);
        ox.append(xs[a]); oy.append(ys[a]);
        if (b != a) { ox.append(xs[b]); oy.append(ys[b]); }
    }
}
}

SteeringChart::SteeringChart(QWidget* parent)
    : ChartView(parent)
{
    axXId_ = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0, true });
    int axPct = addAxis({ Side::Left, -1.0, 1.0, QColor(), true,  'f', 0 });

    setAxisTimeTicker(axXId_, "%m:%s");

    stId_ = addSeries({ "Steering", C_STEER, 2.0, axXId_, axPct, "", 0, false });

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
void SteeringChart::setWindowSeconds(float seconds) { windowS_ = seconds; requestRefresh(); }
void SteeringChart::requestRefresh() { dirty_ = true; }

float SteeringChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void SteeringChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    const int cols = std::max(width(), 400);
    QVector<double> ox, oy;
    decimate(xs, ys, cols, ox, oy);
    setSeriesData(id, ox, oy);
}

void SteeringChart::refresh() {
    if (!model_) return;
    buildDefault(currentTime());
    requestReplot();
}

void SteeringChart::buildDefault(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    QVector<double> tx, st;
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t,
            [](const auto& s, float key) { return s.t < key; });
    };
    for (auto it = lb(d.telBuf, left); it != d.telBuf.end() && it->t <= endTime; ++it) {
        tx.append(it->t); st.append(it->steering);
    }
    putSeries(stId_, tx, st);

    const double lo = tx.isEmpty() ? 0.0 : std::max<double>(left, tx.first());
    const double hi = tx.isEmpty() ? windowS_ : endTime;
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}
