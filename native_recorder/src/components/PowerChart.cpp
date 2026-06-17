#include "PowerChart.h"
#include "../SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_ICE("#5794F2"), C_MGUK("#FADE2A"), C_HARV_K("#37872D"), C_HARV_H("#C4162A"), C_FUEL("#F0A500");
}

PowerChart::PowerChart(PowerChartType type, QWidget* parent)
    : ChartView(parent), type_(type)
{
    axXId_ = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0, true });
    
    int axYId = -1;
    if (type_ == PowerChartType::Split) {
        axYId = addAxis({ Side::Left, 0.0, 1000.0, QColor(), true,  'f', 0 });
        s1Id_ = addSeries({ "ICE",   C_ICE,  1.5, axXId_, axYId, "", 2, false, true });
        s2Id_ = addSeries({ "MGU-K", C_MGUK, 1.5, axXId_, axYId, "", 2, false, true });
    } else if (type_ == PowerChartType::Harvest) {
        // Harvest in kJ, maybe 0-4000? Let's just auto range or 0-4000
        axYId = addAxis({ Side::Left, 0.0, 4000.0, QColor(), true,  'f', 0 });
        s1Id_ = addSeries({ "MGU-K Harvest", C_HARV_K, 1.5, axXId_, axYId, "", 2, false, true });
        s2Id_ = addSeries({ "MGU-H Harvest", C_HARV_H, 1.5, axXId_, axYId, "", 2, false, true });
    } else if (type_ == PowerChartType::Store) {
        axYId = addAxis({ Side::Left, 0.0, 100.0, QColor(), true,  'f', 0 });
        s1Id_ = addSeries({ "ERS Store", C_ICE, 1.5, axXId_, axYId, "", 2, false, true });
    } else if (type_ == PowerChartType::Fuel) {
        axYId = addAxis({ Side::Left, 0.0, 110.0, QColor(), true,  'f', 0 });
        s1Id_ = addSeries({ "Fuel", C_FUEL, 1.5, axXId_, axYId, "", 2, false, true });
    }

    setAxisTimeTicker(axXId_, "%m:%s");
    setHoverReadout(true);
    setLegendVisible(false);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
    refreshTimer_->start();
}

void PowerChart::showEvent(QShowEvent* e) {
    ChartView::showEvent(e);
    requestRefresh();
}

void PowerChart::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &PowerChart::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &PowerChart::requestRefresh);
    requestRefresh();
}

void PowerChart::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void PowerChart::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void PowerChart::setWindowSeconds(float seconds) { windowS_ = seconds; prevEndTime_ = -9999.0f; requestRefresh(); }
void PowerChart::requestRefresh() { dirty_ = true; }

float PowerChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void PowerChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    setSeriesData(id, xs, ys);
}

void PowerChart::refresh() {
    if (!model_) return;
    if (type_ == PowerChartType::Split) buildSplit(currentTime());
    else if (type_ == PowerChartType::Harvest) buildHarvest(currentTime());
    else if (type_ == PowerChartType::Store) buildStore(currentTime());
    else if (type_ == PowerChartType::Fuel) buildFuel(currentTime());
    requestReplot();
}

void PowerChart::buildSplit(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(s1Id_); clear(s2Id_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.stsBuf.begin(), lb(d.stsBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.stsBuf.size(); ++i) {
        const auto& s = d.stsBuf[i];
        if (s.t > endTime) break;
        appendPoint(s1Id_, s.t, s.ice_kw);
        appendPoint(s2Id_, s.t, s.mguk_kw);
        lastAddedTime_ = s.t;
    }
    trimBefore(s1Id_, left); trimBefore(s2Id_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}

void PowerChart::buildHarvest(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(s1Id_); clear(s2Id_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.stsBuf.begin(), lb(d.stsBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.stsBuf.size(); ++i) {
        const auto& s = d.stsBuf[i];
        if (s.t > endTime) break;
        appendPoint(s1Id_, s.t, s.mguk_harvest_j / 1000.0f); // convert to kJ
        appendPoint(s2Id_, s.t, s.mguh_harvest_j / 1000.0f); // convert to kJ
        lastAddedTime_ = s.t;
    }
    trimBefore(s1Id_, left); trimBefore(s2Id_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}

void PowerChart::buildStore(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(s1Id_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.stsBuf.begin(), lb(d.stsBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.stsBuf.size(); ++i) {
        const auto& s = d.stsBuf[i];
        if (s.t > endTime) break;
        appendPoint(s1Id_, s.t, s.ers); // ers %
        lastAddedTime_ = s.t;
    }
    trimBefore(s1Id_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}

void PowerChart::buildFuel(float endTime) {
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(s1Id_);
        lastAddedTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.stsBuf.begin(), lb(d.stsBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.stsBuf.size(); ++i) {
        const auto& s = d.stsBuf[i];
        if (s.t > endTime) break;
        appendPoint(s1Id_, s.t, s.fuel_kg);
        lastAddedTime_ = s.t;
    }
    trimBefore(s1Id_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}
