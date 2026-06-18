#include "RideHeightChart.h"
#include "../SessionModel.h"

#include <QTimer>

RideHeightChart::RideHeightChart(QWidget* parent) : ChartView(parent) {
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, &RideHeightChart::refresh);

    ChartView::AxisSpec xAx;
    xAx.side = ChartView::Side::Bottom;
    xAx.grid = true;
    xAx.numberFormat = 'f';
    xAx.precision = 1;
    axXId_ = addAxis(xAx);
    setAxisTimeTicker(axXId_, "%m:%s");

    ChartView::AxisSpec yAx;
    yAx.side = ChartView::Side::Left;
    yAx.min = 0.0;
    yAx.max = 150.0;
    yAx.grid = true;
    int axYId = addAxis(yAx);
    setAxisNumberSuffix(axYId, 1.0, " mm", 10.0);

    ChartView::SeriesSpec frontSpec;
    frontSpec.name = "Front";
    frontSpec.color = QColor("#73BF69");
    frontSpec.xAxisId = axXId_;
    frontSpec.yAxisId = axYId;
    frontSpec.unit = " mm";
    frontSpec.tipPrecision = 1;
    frontId_ = addSeries(frontSpec);

    ChartView::SeriesSpec rearSpec;
    rearSpec.name = "Rear";
    rearSpec.color = QColor("#B877DB");
    rearSpec.xAxisId = axXId_;
    rearSpec.yAxisId = axYId;
    rearSpec.unit = " mm";
    rearSpec.tipPrecision = 1;
    rearId_ = addSeries(rearSpec);

    setHoverReadout(true);
}

void RideHeightChart::setModel(SessionModel* m) {
    if (model_) {
        disconnect(model_, nullptr, this, nullptr);
    }
    model_ = m;
    if (model_) {
        connect(model_, &SessionModel::telemetryAppended, this, &RideHeightChart::requestRefresh);
        connect(model_, &SessionModel::wasReset, this, [this] {
            lastAddedTime_ = -1.0f;
            prevEndTime_ = -1.0f;
            clearAll();
            requestRefresh();
        });
    }
    requestRefresh();
}

void RideHeightChart::setPlaybackMode(bool on) {
    if (playback_ == on) return;
    playback_ = on;
    lastAddedTime_ = -1.0f;
    prevEndTime_ = -1.0f;
    requestRefresh();
}

void RideHeightChart::setWindowSeconds(float seconds) {
    if (qFuzzyCompare(windowS_, seconds)) return;
    windowS_ = seconds;
    prevEndTime_ = -1.0f;
    requestRefresh();
}

void RideHeightChart::setCurrentTime(float t) {
    if (qFuzzyCompare(currentTime_, t)) return;
    currentTime_ = t;
    if (playback_) requestRefresh();
}

void RideHeightChart::showEvent(QShowEvent* e) {
    ChartView::showEvent(e);
    requestRefresh();
}

void RideHeightChart::requestRefresh() {
    if (!isVisible()) return;
    dirty_ = true;
    if (!refreshTimer_->isActive()) {
        refreshTimer_->start();
    }
}

float RideHeightChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void RideHeightChart::refresh() {
    if (!dirty_ || !model_) return;
    dirty_ = false;

    float cTime = currentTime();
    if (cTime < prevEndTime_) {
        lastAddedTime_ = -1.0f;
        clearAll();
    }
    
    buildDefault(cTime);

    float startTime = qMax(0.0f, cTime - windowS_);
    setXRange(axXId_, startTime, startTime + windowS_);
    requestReplot();
}

void RideHeightChart::buildDefault(float endTime) {
    const auto& buf = model_->data().motionExBuf;
    if (buf.isEmpty()) return;

    int startIndex = 0;
    if (lastAddedTime_ >= 0) {
        auto it = std::lower_bound(buf.begin(), buf.end(), lastAddedTime_,
            [](const MotionExSample& s, float t) { return s.t < t; });
        startIndex = std::distance(buf.begin(), it);
    }

    if (startIndex < buf.size()) {
        QVector<double> ts, fronts, rears;
        ts.reserve(buf.size() - startIndex);
        fronts.reserve(buf.size() - startIndex);
        rears.reserve(buf.size() - startIndex);

        for (int i = startIndex; i < buf.size(); ++i) {
            const auto& s = buf[i];
            if (s.t > endTime) break;
            ts.push_back(s.t);
            fronts.push_back(s.front_aero);
            rears.push_back(s.rear_aero);
            lastAddedTime_ = s.t;
        }

        if (!ts.isEmpty()) {
            putSeries(frontId_, ts, fronts);
            putSeries(rearId_, ts, rears);
        }
    }

    float startTime = qMax(0.0f, endTime - windowS_);
    trimBefore(frontId_, startTime);
    trimBefore(rearId_, startTime);
    prevEndTime_ = endTime;
}

void RideHeightChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    if (lastAddedTime_ < 0) {
        setSeriesData(id, xs, ys);
    } else {
        for (int i = 0; i < xs.size(); ++i) {
            appendPoint(id, xs[i], ys[i]);
        }
    }
}
